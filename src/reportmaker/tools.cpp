#include "reportmaker/tools.hpp"
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
                    "Deep web research on a topic. Searches DuckDuckGo (general web), "
                    "Wikipedia (factual background), HackerNews (tech news and discussions), "
                    "StackExchange (technical Q&A), and arXiv (academic papers and research). "
                    "Returns a markdown digest of the top sources. "
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

// ── Wikipedia ────────────────────────────────────────────────────────────────

static std::vector<SearchHit> search_wikipedia(const std::string& query, int limit) {
    HttpClient http;
    std::string url = "https://en.wikipedia.org/w/api.php?action=query&list=search"
                      "&srsearch=" + http.url_encode(query) +
                      "&srlimit=" + std::to_string(limit) +
                      "&format=json";

    auto resp = http.get(url, {{"Accept", "application/json"}});
    if (!resp.ok()) return {};

    std::vector<SearchHit> hits;
    try {
        auto j = json::parse(resp.body);
        for (auto& item : j["query"]["search"]) {
            SearchHit h;
            h.title   = item.value("title", "");
            h.snippet = trim(strip_html_tags(item.value("snippet", "")));
            h.url    = "https://en.wikipedia.org/wiki/" + http.url_encode(h.title);
            h.source = "wikipedia";
            if (!h.title.empty()) hits.push_back(std::move(h));
        }
    } catch (...) {}
    return hits;
}

// ── HackerNews (Algolia) ─────────────────────────────────────────────────────

static std::vector<SearchHit> search_hackernews(const std::string& query, int limit) {
    HttpClient http;
    std::string url = "https://hn.algolia.com/api/v1/search?query=" +
                      http.url_encode(query) +
                      "&hitsPerPage=" + std::to_string(limit) +
                      "&tags=story";

    auto resp = http.get(url);
    if (!resp.ok()) return {};

    std::vector<SearchHit> hits;
    try {
        auto j = json::parse(resp.body);
        for (auto& hit : j["hits"]) {
            SearchHit h;
            h.title   = hit.value("title", "");
            h.snippet = hit.contains("story_text") && !hit["story_text"].is_null()
                        ? hit["story_text"].get<std::string>().substr(0, 300) : "";
            h.url     = hit.contains("url") && !hit["url"].is_null()
                        ? hit["url"].get<std::string>()
                        : "https://news.ycombinator.com/item?id=" + hit.value("objectID", "");
            h.source  = "hackernews";
            if (!h.title.empty()) hits.push_back(std::move(h));
        }
    } catch (...) {}
    return hits;
}

// ── StackExchange ─────────────────────────────────────────────────────────────

static std::vector<SearchHit> search_stackexchange(const std::string& query, int limit) {
    HttpClient http;
    std::string url = "https://api.stackexchange.com/2.3/search/advanced"
                      "?q=" + http.url_encode(query) +
                      "&site=stackoverflow"
                      "&pagesize=" + std::to_string(std::min(limit, 25)) +
                      "&order=desc&sort=relevance&filter=withbody";

    auto resp = http.get(url);
    if (!resp.ok()) return {};

    std::vector<SearchHit> hits;
    try {
        auto j = json::parse(resp.body);
        for (auto& item : j["items"]) {
            SearchHit h;
            h.title   = trim(strip_html_tags(item.value("title", "")));
            std::string body = item.value("body", "");
            h.snippet = trim(strip_html_tags(body)).substr(0, 300);
            h.url     = item.value("link", "");
            h.source  = "stackexchange";
            if (!h.url.empty()) hits.push_back(std::move(h));
        }
    } catch (...) {}
    return hits;
}

// ── arXiv ─────────────────────────────────────────────────────────────────────
// Uses the public Atom API — no key required.
// https://export.arxiv.org/api/query?search_query=all:<q>&max_results=<n>

static std::string extract_xml_tag(const std::string& xml,
                                   const std::string& tag,
                                   std::string::size_type from = 0) {
    std::string open  = "<" + tag;
    std::string close = "</" + tag + ">";
    auto s = xml.find(open, from);
    if (s == std::string::npos) return "";
    auto s2 = xml.find('>', s);
    if (s2 == std::string::npos) return "";
    auto e = xml.find(close, s2);
    if (e == std::string::npos) return "";
    return xml.substr(s2 + 1, e - s2 - 1);
}

static std::vector<SearchHit> search_arxiv(const std::string& query, int limit) {
    HttpClient http;
    http.timeout_sec = 15;

    std::string url = "https://export.arxiv.org/api/query?search_query=all:"
                      + http.url_encode(query)
                      + "&max_results=" + std::to_string(std::min(limit, 10))
                      + "&sortBy=relevance";

    auto resp = http.get(url, {{"Accept", "application/atom+xml"}});
    if (!resp.ok()) return {};

    std::vector<SearchHit> hits;
    const std::string& body = resp.body;
    std::string::size_type pos = 0;

    while (hits.size() < (size_t)limit) {
        auto entry_start = body.find("<entry>", pos);
        if (entry_start == std::string::npos) break;
        auto entry_end = body.find("</entry>", entry_start);
        if (entry_end == std::string::npos) break;

        std::string entry = body.substr(entry_start, entry_end - entry_start + 8);
        pos = entry_end + 8;

        SearchHit h;
        h.title   = trim(extract_xml_tag(entry, "title"));
        h.snippet = trim(extract_xml_tag(entry, "summary"));
        if (h.snippet.size() > 500) h.snippet = h.snippet.substr(0, 500) + "...";

        // Prefer the abs link
        std::string::size_type lpos = 0;
        while (true) {
            auto lk = entry.find("<link", lpos);
            if (lk == std::string::npos) break;
            auto lend = entry.find("/>", lk);
            if (lend == std::string::npos) break;
            std::string ltag = entry.substr(lk, lend - lk + 2);
            if (ltag.find("rel=\"alternate\"") != std::string::npos ||
                ltag.find("type=\"text/html\"") != std::string::npos) {
                auto href = ltag.find("href=\"");
                if (href != std::string::npos) {
                    auto vs = href + 6;
                    auto ve = ltag.find('"', vs);
                    h.url = ltag.substr(vs, ve - vs);
                }
                break;
            }
            lpos = lend + 2;
        }
        // Fallback to <id> which is also a valid URL
        if (h.url.empty()) h.url = trim(extract_xml_tag(entry, "id"));

        h.source = "arxiv";
        if (!h.title.empty() && !h.url.empty())
            hits.push_back(std::move(h));
    }
    return hits;
}

// ── Tool dispatch ─────────────────────────────────────────────────────────────

static std::string run_smart_search(const json& args) {
    std::string topic = args.value("topic", "");
    int limit         = args.value("limit", 8);
    std::string region = args.value("region", "us-en");

    if (topic.empty()) return "ERROR: topic is required";

    // Search all five sources
    auto ddg   = search_duckduckgo(topic, limit, region);
    auto wiki  = search_wikipedia(topic, limit / 2 + 1);
    auto hn    = search_hackernews(topic, limit / 2);
    auto se    = search_stackexchange(topic, limit / 2);
    auto ax    = search_arxiv(topic, 5);

    // Merge with URL deduplication
    std::vector<SearchHit> all;
    std::set<std::string>  seen_urls;
    auto dedup_merge = [&](std::vector<SearchHit>& src) {
        for (auto& h : src)
            if (!h.url.empty() && seen_urls.insert(h.url).second)
                all.push_back(h);
    };
    dedup_merge(ddg);
    dedup_merge(wiki);
    dedup_merge(hn);
    dedup_merge(se);
    dedup_merge(ax);

    if (all.empty()) return "smart_search: no results found for: " + topic;

    // Build markdown digest
    std::ostringstream out;
    out << "## Smart Search Digest: " << topic << "\n\n";
    out << "_" << all.size() << " results from duckduckgo / wikipedia / hackernews / stackexchange / arxiv_\n\n";

    for (auto& h : all) {
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

    // Top web results for context
    auto hits = search_duckduckgo(query, 5, "us-en");
    if (!hits.empty()) {
        out << "**Top web results:**\n\n";
        for (auto& h : hits) {
            out << "- [" << (h.title.empty() ? h.url : h.title) << "](" << h.url << ")\n";
            if (!h.snippet.empty()) out << "  " << h.snippet << "\n";
        }
    }

    return out.str();
}

std::string execute_tool(const std::string& name, const json& args) {
    if (name == "smart_search") return run_smart_search(args);
    if (name == "fact_check")   return run_fact_check(args);
    return "ERROR: unknown tool '" + name + "'";
}

} // namespace reportmaker
