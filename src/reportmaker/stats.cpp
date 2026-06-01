#include "reportmaker/stats.hpp"
#include "config.hpp"
#include <fstream>
#include <chrono>
#include <ctime>
#include <algorithm>

using json = nlohmann::json;

namespace reportmaker {

// ── Source metadata — platform-specific limits ────────────────────────────────
// Sources: DDG HTML scrape, Wikipedia API, HackerNews/Algolia, StackExchange, arXiv.

const std::vector<std::pair<std::string, SourceMeta>>& source_meta_list() {
    static const std::vector<std::pair<std::string, SourceMeta>> list = {
        {"duckduckgo",    {  60,    30, "min", "unofficial soft limit"}},
        {"wikipedia",     {  60,   500, "min", "API docs ~500 req/min"}},
        {"hackernews",    {  60,    60, "min", "Algolia 1 req/sec"}},
        {"stackexchange", {86400,  300, "day", "300 req/day (no key)"}},
        {"arxiv",         {  60,   180, "min", "3 req/sec guideline"}},
    };
    return list;
}

static const SourceMeta* find_meta(const std::string& source) {
    for (auto& [name, meta] : source_meta_list())
        if (name == source) return &meta;
    return nullptr;
}

// ── Singleton ─────────────────────────────────────────────────────────────────

ServerStats& ServerStats::get() {
    static ServerStats inst;
    return inst;
}

ServerStats::ServerStats() : started_at_(std::chrono::system_clock::now()) {
    for (auto& [name, _] : source_meta_list())
        sources_[name] = SourceState{};
}

// ── Recording ─────────────────────────────────────────────────────────────────

void ServerStats::record_source(const std::string& source) {
    auto* meta = find_meta(source);
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = sources_[source];
    ++st.total;
    int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    st.ts.push_back(now_sec);
    // Prune entries older than 2× window
    if (meta) {
        int64_t cutoff = now_sec - 2 * meta->window_sec;
        while (!st.ts.empty() && st.ts.front() < cutoff)
            st.ts.pop_front();
    }
}

void ServerStats::record_tool(const std::string& tool_name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (tool_name == "smart_search") ++ss_calls_;
    else if (tool_name == "fact_check") ++fc_calls_;
}

void ServerStats::record_report() {
    std::lock_guard<std::mutex> lk(mu_);
    ++reports_served_;
}

// ── Window usage ──────────────────────────────────────────────────────────────

int ServerStats::window_usage(const std::string& source) const {
    auto* meta = find_meta(source);
    if (!meta) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sources_.find(source);
    if (it == sources_.end()) return 0;
    int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t cutoff = now_sec - meta->window_sec;
    int count = 0;
    for (auto t : it->second.ts)
        if (t >= cutoff) ++count;
    return count;
}

// ── JSON snapshot ─────────────────────────────────────────────────────────────

static std::string epoch_to_iso(int64_t sec) {
    std::time_t t = (std::time_t)sec;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

nlohmann::json ServerStats::to_json() const {
    std::lock_guard<std::mutex> lk(mu_);

    int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t start_sec = std::chrono::duration_cast<std::chrono::seconds>(
        started_at_.time_since_epoch()).count();

    json j;
    j["started_at"]     = epoch_to_iso(start_sec);
    j["uptime_seconds"] = now_sec - start_sec;
    j["reports_served"] = reports_served_;
    j["tool_calls"]     = {{"smart_search", ss_calls_}, {"fact_check", fc_calls_}};

    json sources_j = json::object();
    for (auto& [name, meta] : source_meta_list()) {
        auto it = sources_.find(name);
        int64_t cutoff = now_sec - meta.window_sec;
        int used = 0;
        long long total = 0;
        if (it != sources_.end()) {
            total = it->second.total;
            for (auto t : it->second.ts)
                if (t >= cutoff) ++used;
        }
        double pct = meta.limit > 0 ? (100.0 * used / meta.limit) : 0.0;
        sources_j[name] = {
            {"window_sec",    meta.window_sec},
            {"window_label",  meta.window_label},
            {"limit",         meta.limit},
            {"note",          meta.note},
            {"used_in_window", used},
            {"total",         total},
            {"pct",           pct}
        };
    }
    j["sources"] = sources_j;
    return j;
}

// ── Persistence ───────────────────────────────────────────────────────────────

void ServerStats::flush() const {
    try {
        auto path = stats_path();
        std::ofstream f(path);
        f << to_json().dump(2) << "\n";
    } catch (...) {}
}

nlohmann::json load_stats_file() {
    try {
        std::ifstream f(stats_path());
        if (!f) return json::object();
        return json::parse(f);
    } catch (...) {
        return json::object();
    }
}

} // namespace reportmaker
