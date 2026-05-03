#pragma once
#include <string>

namespace reportmaker {

constexpr int MAX_WARNINGS = 2;

struct Budget {
    // Limits — populated from Config at run_report() time
    int ss_min = 2;
    int ss_max = 5;
    int fc_min = 3;
    int fc_max = 8;

    int smart_search_count     = 0;
    int fact_check_count       = 0;
    int iterations_used        = 0;
    int budget_warnings_issued = 0;

    void record_tool(const std::string& tool_name);
    std::string underspent() const;
    bool would_exceed(const std::string& tool_name) const;
    bool hard_cap_reached() const;
    std::string summary() const;
};

} // namespace reportmaker
