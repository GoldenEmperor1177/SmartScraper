#include "reportmaker/tools.hpp"
#include "reportmaker/stats.hpp"
#include "http_client.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <regex>
#include <algorithm>
#include <iostream>
#include <set>

using json = nlohmann::json;

// ── Tool schemas (OpenAI / DeepSeek format) ──────────────────────────────────

namespace reportmaker {

json tool_schemas() {
    return json::array({
        {
            {"type", "function"},
            {"function", {
                {"name", "smart_search"},
                {"description",
                    "Deep web research on a topic. Searches DuckDuckGo across the open web — "
                    "finds news, blogs, market reports, industry data, and reference pages. "
                    "Returns a markdown digest of the top sources with titles, URLs, and snippets. "
                    "Slower (10-30s) but comprehensive. Use for whole subjects, not single facts."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"topic", {
                            {"type", "string"},
                            {"description", "The research topic. Be specific; broad topics return shallow results."}
                        }},
                        {"limit", {
                            {"type", "integer"},
                            {"description", "Max results per source (default 8). Lower = faster."}
                        }},
                        {"region", {
                            {"type", "string"},
                            {"description", "Locale for DuckDuckGo: us-en, in-en, gb-en, wt-wt (worldwide)."}
                        }}
                    }},
                    {"required", json::array({"topic"})}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "fact_check"},
                {"description",
                    "Fast fact retrieval. Use to verify single numbers or stats: conversion rates, "
                    "salaries, market sizes, prices, CAGR. Returns top results + sources. "
                    "Always prefer this over guessing a number."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"query", {
                            {"type", "string"},
                            {"description", "Specific factual question, e.g. 'average B2B SaaS conversion rate'."}
                        }}
                    }},
                    {"required", json::array({"query"})}
                }}
            }}
        }
    });
}

// ── HTML helpers ─────────────────────────────────────────────────────────────

static std::string strip_html_tags(const std::string& s) {
    return std::regex_replace(s, std::regex("<[^>]+>"), "");
}

static std::string trim(std::string s) {
    auto l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return "";
    auto r = s.find_last_not_of(" \t\r\n");
    return s.substr(l, r - l + 1);
}

// Extract the real destination URL from a DuckDuckGo redirect href.
// DDG often returns //duckduckgo.com/l/?uddg=<encoded>&... — we decode uddg.
static std::string extract_ddg_url(const std::string& href, HttpClient& http) {
    if (href.find("duckduckgo.com/l/") != std::string::npos ||
        href.find("//duckduckgo.com/l?") != std::string::npos) {
        auto pos = href.find("uddg=");
        if (pos != std::string::npos) {
            auto end = href.find('&', pos + 5);
            auto encoded = href.substr(pos + 5,
                end == std::string::npos ? std::string::npos : end - pos - 5);
            return http.url_decode(encoded);
        }
    }
    if (href.substr(0, 2) == "//") return "https:" + href;
    return href;
}

struct SearchHit {
    std::string url;
    std::string title;
    std::string snippet;
    std::string source;
};

// ── DuckDuckGo ───────────────────────────────────────────────────────────────

static std::vector<SearchHit> search_duckduckgo(
        const std::string& query, int limit, const std::string& region) {

    HttpClient http;
    http.timeout_sec = 20;
    std::string body = "q=" + http.url_encode(query) + "&kl=" + region;

    auto resp = http.post(
        "https://html.duckduckgo.com/html/",
        body,
        "application/x-www-form-urlencoded",
        {{"Accept-Language", "en-US,en;q=0.9"}}
    );

    if (!resp.ok()) return {};

    std::vector<SearchHit> hits;
    const std::string& html = resp.body;

    // Find result__a links and their following snippets
    std::regex link_re(R"re(class="result__a"[^>]+href="([^"]+)"[^>]*>(.*?)</a>)re",
                       std::regex::icase | std::regex::multiline);
    std::regex snip_re(R"re(class="result__snippet"[^>]*>(.*?)</)re",
                       std::regex::icase | std::regex::multiline);

    auto links_begin = std::sregex_iterator(html.begin(), html.end(), link_re);
    auto snips_begin = std::sregex_iterator(html.begin(), html.end(), snip_re);
    auto end         = std::sregex_iterator();

    std::vector<std::pair<std::string,std::string>> links; // (href, title)
    for (auto it = links_begin; it != end && (int)links.size() < limit; ++it)
        links.push_back({(*it)[1].str(), trim(strip_html_tags((*it)[2].str()))});

    std::vector<std::string> snippets;
    for (auto it = snips_begin; it != end && (int)snippets.size() < limit; ++it)
        snippets.push_back(trim(strip_html_tags((*it)[1].str())));

    for (int i = 0; i < (int)links.size(); ++i) {
        SearchHit h;
        h.url     = extract_ddg_url(links[i].first, http);
        h.title   = links[i].second;
        h.snippet = i < (int)snippets.size() ? snippets[i] : "";
        h.source  = "duckduckgo";
        if (!h.url.empty() && h.url.substr(0, 4) == "http")
            hits.push_back(std::move(h));
    }
    return hits;
}

// ── Tool dispatch ─────────────────────────────────────────────────────────────

static std::string run_smart_search(const json& args) {
    std::string topic = args.value("topic", "");
    int limit         = args.value("limit", 8);
    // Default to worldwide so regional queries (India, etc.) aren't geo-filtered.
    // Caller passes "in-en" for India-specific queries.
    std::string region = args.value("region", "wt-wt");

    if (topic.empty()) return "ERROR: topic is required";

    // DuckDuckGo only: Wikipedia/HackerNews/StackExchange/arXiv return encyclopedic
    // and tech content that is irrelevant for business, marketing, and domain-specific
    // research queries. They pollute the digest and cause the agent to report "no results".
    auto& stats = ServerStats::get();
    auto ddg = search_duckduckgo(topic, limit, region);
    stats.record_source("duckduckgo");
    stats.flush();

    if (ddg.empty()) return "smart_search: no results found for: " + topic;

    // Build markdown digest from DuckDuckGo results
    std::ostringstream out;
    out << "## Smart Search Digest: " << topic << "\n\n";
    out << "_" << ddg.size() << " results from duckduckgo_\n\n";

    for (auto& h : ddg) {
        out << "### [" << (h.title.empty() ? h.url : h.title) << "](" << h.url << ")\n";
        out << "_source: " << h.source << "_\n\n";
        if (!h.snippet.empty()) out << h.snippet << "\n\n";
    }

    std::string result = out.str();
    if (result.size() > 20000) result = result.substr(0, 20000) + "\n\n[truncated]";
    return result;
}

static std::string run_fact_check(const json& args) {
    std::string query = args.value("query", "");
    if (query.empty()) return "ERROR: query is required";

    // Fast path: DuckDuckGo instant answer + top 3 web hits
    HttpClient http;
    std::string ia_url = "https://api.duckduckgo.com/?q=" +
                         http.url_encode(query) +
                         "&format=json&no_html=1&skip_disambig=1";

    std::ostringstream out;
    out << "## Fact Check: " << query << "\n\n";

    auto ia_resp = http.get(ia_url);
    if (ia_resp.ok() && !ia_resp.body.empty()) {
        try {
            auto j = json::parse(ia_resp.body);
            std::string abstract = j.value("Abstract", "");
            std::string abstract_url = j.value("AbstractURL", "");
            if (!abstract.empty()) {
                out << "**Instant Answer:** " << abstract << "\n";
                if (!abstract_url.empty()) out << "**Source:** " << abstract_url << "\n\n";
            }
        } catch (...) {}
    }

    // Top web results — use worldwide region so India/regional queries aren't filtered
    auto hits = search_duckduckgo(query, 5, "wt-wt");
    ServerStats::get().record_source("duckduckgo");
    ServerStats::get().flush();
    if (!hits.empty()) {
        out << "**Top web results:**\n\n";
        for (auto& h : hits) {
            out << "- [" << (h.title.empty() ? h.url : h.title) << "](" << h.url << ")\n";
            if (!h.snippet.empty()) out << "  " << h.snippet << "\n";
        }
    } else {
        out << "_No web results returned for this query._\n";
    }

    return out.str();
}

std::string execute_tool(const std::string& name, const json& args) {
    if (name == "smart_search") return run_smart_search(args);
    if (name == "fact_check")   return run_fact_check(args);
    return "ERROR: unknown tool '" + name + "'";
}

} // namespace reportmaker
