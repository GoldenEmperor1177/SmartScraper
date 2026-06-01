#pragma once
#include <string>
#include <map>
#include <deque>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

namespace reportmaker {

// Known per-platform limits for each search source.
struct SourceMeta {
    int         window_sec;     // rolling window in seconds
    int         limit;          // max calls allowed in that window
    std::string window_label;   // "min", "day", "sec" — for display
    std::string note;           // e.g. "unofficial soft limit"
};

// Per-source rolling call history (in-memory).
struct SourceState {
    long long total = 0;
    // unix epoch seconds of recent calls; pruned to 2× window_sec
    std::deque<int64_t> ts;
};

// Process-wide stats — lives in the server process (or current CLI run).
class ServerStats {
public:
    static ServerStats& get();   // singleton

    void record_source(const std::string& source);
    void record_tool(const std::string& tool_name);   // "smart_search" | "fact_check"
    void record_report();

    // Window usage (calls in the last window_sec for that source).
    int  window_usage(const std::string& source) const;

    // Full JSON snapshot — used by /stats endpoint and written to stats.json.
    nlohmann::json to_json() const;

    // Persist snapshot to ~/.smartscraper/stats.json.
    void flush() const;

private:
    ServerStats();
    mutable std::mutex mu_;
    std::chrono::system_clock::time_point started_at_;
    long long reports_served_ = 0;
    long long ss_calls_       = 0;
    long long fc_calls_       = 0;
    std::map<std::string, SourceState> sources_;
};

// Metadata table — same order used in display.
const std::vector<std::pair<std::string, SourceMeta>>& source_meta_list();

// Read the last-flushed stats file (for rp stats CLI when server isn't in-process).
nlohmann::json load_stats_file();

} // namespace reportmaker
