#include "reportmaker/agent.hpp"
#include "reportmaker/budget.hpp"
#include "reportmaker/tools.hpp"
#include "http_client.hpp"
#include "config.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <ctime>
#include <set>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace reportmaker {

// ── System prompt ──────────────────────────────────────────────────────────────

static const char* SYSTEM_PROMPT = R"(You are a senior research analyst. Your job is NOT to write fast, fluent reports. Your job is to write reports where every single claim is traceable to a source. If you cannot ground a claim, you do not write it. Period.

# THE GROUNDING RULE (the most important rule)

EVERY number, percentage, dollar amount, growth rate, salary figure, market size, conversion rate, time-to-X estimate, ranking, or comparative claim MUST come from a tool call in THIS conversation. Not from your training data. Not from "common knowledge."

If you find yourself about to type a number, STOP. Ask:
1. Did a tool in this conversation return this number?
2. Can I cite the exact source URL?

If either answer is no, you have two choices:
- Call `fact_check` to ground it RIGHT NOW
- Remove the number entirely and replace with a qualitative description

There is no third option.

# RESEARCH DEPTH

Your minimum protocol:
1. Start with 1-2 `smart_search` calls on the broad topic
2. Then run `fact_check` on key numbers you found
3. If you discover major sub-topics, do 1 more `smart_search` each
4. If two sources disagree, run one more `fact_check` to break the tie

Maximum budget (hard limit):
- At most 5 `smart_search` calls total
- At most 8 `fact_check` calls total
- After that, synthesise the final report with what you have

# CONSERVATIVE STANCE

You are an analyst, not a hype-man. NEVER write optimistic projections without explicit caveats.

# MANDATORY DISCLAIMER BLOCK

Every final report MUST end with:

## Disclaimers

- **Observed patterns, not predictions.** Every number in this report describes what specific sources observed in specific markets.
- **Survivorship bias is everywhere.** Public benchmarks over-represent companies that succeeded.
- **Sources are not equal.** Vendor blogs and consultancy reports have incentives to inflate market sizes.
- **Time decay.** Market data ages fast. Numbers older than 12-18 months may already be wrong.
- **Your mileage will vary.** Average outcomes describe a distribution. Individual outcomes routinely fall outside the average.
- **No financial, legal, or investment advice.** This is research, not counsel.

# CITATION FORMAT

After every grounded claim, append: `[source: <short identifier> — <URL>]`

# REPORT STRUCTURE (MANDATORY)

## SECTION 1: EVIDENCE LAYER — Facts, Sources & Provenance
Only directly observed, sourced facts from tool calls. No editorializing.

## SECTION 2: ANALYSIS LAYER — Synthesis, Estimates & Actionability
1. TL;DR — 3-5 bullets
2. Key Insights
3. Conservative Estimates
4. Where the Data Disagrees
5. What We Could NOT Verify (REQUIRED — never skip)
6. Actionable Recommendations
7. Disclaimers

Output the final report as markdown. No preamble — start directly with "## SECTION 1: EVIDENCE LAYER".)";

static const char* SUMMARY_PROMPT = R"(Summarize the following research report in 150-200 words.
Focus on: what topic was covered, what numbers were established, what gaps remain.
This summary will be loaded as context if a related follow-up question is asked.

REPORT:
{report}

Output ONLY the summary, no preamble.)";

// ── Timestamp helper ──────────────────────────────────────────────────────────

static std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

static std::string slugify(const std::string& s, int max_len = 40) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(c)) out += std::tolower(c);
        else if (std::isspace(c) && !out.empty() && out.back() != '-') out += '-';
    }
    if ((int)out.size() > max_len) out = out.substr(0, max_len);
    return out;
}

// ── Report cache ──────────────────────────────────────────────────────────────

static void save_report(const std::string& query, const std::string& report,
                         const std::string& summary, int tool_calls) {
    auto dir = reports_dir();
    fs::create_directories(dir);

    auto ts  = now_iso();
    auto id  = ts.substr(0, 10) + "-" + slugify(query);
    auto path = dir / (id + ".json");

    json j;
    j["id"]             = id;
    j["timestamp"]      = ts;
    j["user_request"]   = query;
    j["final_report"]   = report;
    j["summary"]        = summary;
    j["tool_calls_used"] = tool_calls;

    std::ofstream f(path);
    f << j.dump(2) << "\n";
}

static std::string generate_summary(const std::string& report, const AgentConfig& cfg) {
    // Ask the same model to summarize (short call, no tools)
    std::string prompt = SUMMARY_PROMPT;
    auto pos = prompt.find("{report}");
    if (pos != std::string::npos)
        prompt.replace(pos, 8, report.substr(0, 8000));

    json body = {
        {"model", cfg.model},
        {"max_tokens", 400},
        {"messages", json::array({
            {{"role", "user"}, {"content", prompt}}
        })}
    };

    HttpClient http;
    http.timeout_sec = 60;
    auto resp = http.post(
        cfg.base_url + "/chat/completions",
        body.dump(),
        "application/json",
        {{"Authorization", "Bearer " + cfg.api_key}}
    );

    if (!resp.ok()) return report.substr(0, 500);
    try {
        auto j = json::parse(resp.body);
        return j["choices"][0]["message"]["content"].get<std::string>();
    } catch (...) {
        return report.substr(0, 500);
    }
}

// Simple word-overlap relevance for prior-report context loading
static int word_overlap(const std::string& a, const std::string& b) {
    auto words = [](const std::string& s) {
        std::set<std::string> ws;
        std::istringstream ss(s);
        std::string w;
        while (ss >> w) {
            std::transform(w.begin(), w.end(), w.begin(), ::tolower);
            if (w.size() > 3) ws.insert(w);
        }
        return ws;
    };
    auto wa = words(a), wb = words(b);
    int count = 0;
    for (auto& w : wa) if (wb.count(w)) ++count;
    return count;
}

static std::string load_context(const std::string& query) {
    auto dir = reports_dir();
    if (!fs::exists(dir)) return "";

    struct Entry { int score; std::string summary; std::string request; };
    std::vector<Entry> entries;

    for (auto& p : fs::directory_iterator(dir)) {
        if (p.path().extension() != ".json") continue;
        try {
            std::ifstream f(p.path());
            auto j = json::parse(f);
            std::string req = j.value("user_request", "");
            std::string sum = j.value("summary", "");
            int score = word_overlap(query, req + " " + sum);
            if (score > 2) entries.push_back({score, sum, req});
        } catch (...) {}
    }

    if (entries.empty()) return "";

    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b){ return a.score > b.score; });
    if (entries.size() > 3) entries.resize(3);

    std::ostringstream ctx;
    ctx << "[PRIOR REPORT CONTEXT — already established, do not re-derive]\n\n";
    for (auto& e : entries)
        ctx << "## Prior report: " << e.request << "\n\nSummary: " << e.summary << "\n\n---\n\n";
    ctx << "[END PRIOR CONTEXT]\n\nUser's new request:\n";
    return ctx.str();
}

// ── DeepSeek agent loop ───────────────────────────────────────────────────────

std::string run_report(const std::string& query, const AgentConfig& cfg) {
    if (cfg.api_key.empty()) {
        std::cerr << "[error] No API key configured. Run: rp insert -key <your_key>\n";
        return "";
    }

    // Build initial message
    std::string initial = query;
    if (cfg.use_context) {
        std::string ctx = load_context(query);
        if (!ctx.empty()) {
            if (cfg.verbose) std::cout << "[context] Loaded prior reports as context.\n";
            initial = ctx + query;
        }
    }

    json messages = json::array({
        {{"role", "system"}, {"content", SYSTEM_PROMPT}},
        {{"role", "user"},   {"content", initial}}
    });

    Budget budget;
    budget.ss_min = cfg.ss_min;
    budget.ss_max = cfg.ss_max;
    budget.fc_min = cfg.fc_min;
    budget.fc_max = cfg.fc_max;
    HttpClient http;
    http.timeout_sec = 120;
    std::string api_url = cfg.base_url + "/chat/completions";
    std::string final_text;

    for (int iter = 0; iter < cfg.max_iterations; ++iter) {
        budget.iterations_used = iter + 1;
        if (cfg.verbose)
            std::cout << "\n[iter " << (iter+1) << "/" << cfg.max_iterations
                      << "] calling " << cfg.model << "... (" << budget.summary() << ")\n";

        json req_body = {
            {"model",      cfg.model},
            {"max_tokens", cfg.max_tokens},
            {"tools",      tool_schemas()},
            {"messages",   messages}
        };

        auto resp = http.post(api_url, req_body.dump(), "application/json",
                              {{"Authorization", "Bearer " + cfg.api_key}});

        if (!resp.ok()) {
            std::cerr << "[error] API call failed (" << resp.status_code << "): "
                      << resp.body.substr(0, 300) << "\n";
            break;
        }

        json result;
        try { result = json::parse(resp.body); }
        catch (...) { std::cerr << "[error] Failed to parse API response\n"; break; }

        if (!result.contains("choices") || result["choices"].empty()) {
            std::cerr << "[error] No choices in response\n"; break;
        }

        auto& choice       = result["choices"][0];
        std::string finish = choice.value("finish_reason", "");
        auto& msg          = choice["message"];

        if (finish == "stop") {
            std::string candidate = msg.value("content", "");

            // Hard cap: if limits were already hit, accept regardless
            if (budget.hard_cap_reached()) {
                if (cfg.verbose) std::cout << "[done] Hard cap reached — accepting report.\n";
                final_text = candidate;
                break;
            }

            std::string warning = budget.underspent();
            if (!warning.empty()) {
                ++budget.budget_warnings_issued;
                if (cfg.verbose) std::cout << "[budget] Pushing back: " << warning << "\n";
                messages.push_back({{"role", "assistant"}, {"content", candidate}});
                messages.push_back({{"role", "user"}, {"content",
                    "STOP. You attempted to end the turn but: " + warning + "\n\n"
                    "Run more tool calls to ground the missing claims. "
                    "Then re-emit the complete report with proper citations and the Disclaimers block."}});
                continue;
            }

            if (cfg.verbose)
                std::cout << "[done] finish_reason=stop after " << (iter+1)
                          << " iterations. Budget: " << budget.summary() << "\n";
            final_text = candidate;
            break;
        }

        if (finish == "tool_calls") {
            auto tool_calls = msg.value("tool_calls", json::array());

            // Check hard cap before executing any tool
            bool would_exceed = false;
            for (auto& tc : tool_calls) {
                std::string name = tc["function"].value("name", "");
                if (budget.would_exceed(name)) { would_exceed = true; break; }
            }

            if (would_exceed) {
                if (cfg.verbose) std::cout << "  -> HARD CAP: forcing synthesis\n";
                // Append assistant message with the tool_calls
                messages.push_back(msg);
                // Provide stub tool results (API requires a result per tool_call)
                for (auto& tc : tool_calls) {
                    messages.push_back({
                        {"role", "tool"},
                        {"tool_call_id", tc.value("id", "")},
                        {"content", "[Skipped: budget cap reached. Use data already collected.]"}
                    });
                }
                messages.push_back({{"role", "user"}, {"content",
                    "HARD CAP REACHED. You have called enough tools. "
                    "Current budget: smart_search=" + std::to_string(budget.smart_search_count) +
                    ", fact_check=" + std::to_string(budget.fact_check_count) + ". "
                    "You MUST now synthesize the final report with the data you already have. "
                    "Do not call any more tools. Output the complete report now."}});
                continue;
            }

            // Execute all tools normally
            messages.push_back(msg);
            for (auto& tc : tool_calls) {
                std::string tool_name = tc["function"].value("name", "");
                std::string tool_id   = tc.value("id", "");
                json tool_args;
                try { tool_args = json::parse(tc["function"].value("arguments", "{}")); }
                catch (...) { tool_args = json::object(); }

                budget.record_tool(tool_name);
                if (cfg.verbose) std::cout << "  -> tool: " << tool_name << "\n";
                std::string tool_result = execute_tool(tool_name, tool_args);
                if (cfg.verbose) {
                    auto preview = tool_result.substr(0, 200);
                    std::replace(preview.begin(), preview.end(), '\n', ' ');
                    std::cout << "     result: " << preview << "...\n";
                }
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", tool_id},
                    {"content", tool_result}
                });
            }
            continue;
        }

        // Unexpected finish reason
        std::cerr << "[warn] Unexpected finish_reason: " << finish << "\n";
        final_text = msg.value("content", "");
        break;
    }

    if (final_text.empty())
        return "[max_iterations=" + std::to_string(cfg.max_iterations) +
               " reached. Budget: " + budget.summary() + "]";

    // Cache the report
    if (cfg.use_cache) {
        try {
            std::string summary = generate_summary(final_text, cfg);
            int tool_calls = budget.smart_search_count + budget.fact_check_count;
            save_report(query, final_text, summary, tool_calls);
            if (cfg.verbose) std::cout << "\n[cache] Report saved.\n";
        } catch (const std::exception& e) {
            std::cerr << "[warn] Failed to cache report: " << e.what() << "\n";
        }
    }

    return final_text;
}

} // namespace reportmaker
