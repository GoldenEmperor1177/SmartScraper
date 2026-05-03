#include "reportmaker/budget.hpp"
#include <sstream>

namespace reportmaker {

void Budget::record_tool(const std::string& tool_name) {
    if      (tool_name == "smart_search") ++smart_search_count;
    else if (tool_name == "fact_check")   ++fact_check_count;
}

std::string Budget::underspent() const {
    if (budget_warnings_issued >= MAX_WARNINGS) return "";
    if (smart_search_count < ss_min)
        return "Only " + std::to_string(smart_search_count) +
               " smart_search call(s) so far (minimum " + std::to_string(ss_min) + "). "
               "The report cannot be grounded enough yet. "
               "Run more smart_search calls on sub-topics before writing the final report.";
    if (fact_check_count < fc_min)
        return "Only " + std::to_string(fact_check_count) +
               " fact_check call(s) so far (minimum " + std::to_string(fc_min) + "). "
               "The numbers in the report are not yet verified. "
               "Run fact_check on every key statistic before finalizing.";
    return "";
}

bool Budget::would_exceed(const std::string& tool_name) const {
    if (tool_name == "smart_search") return smart_search_count >= ss_max;
    if (tool_name == "fact_check")   return fact_check_count   >= fc_max;
    return false;
}

bool Budget::hard_cap_reached() const {
    return smart_search_count >= ss_max || fact_check_count >= fc_max;
}

std::string Budget::summary() const {
    std::ostringstream ss;
    ss << "smart_search=" << smart_search_count << "/" << ss_max
       << ", fact_check=" << fact_check_count   << "/" << fc_max
       << ", iterations=" << iterations_used
       << ", warnings="   << budget_warnings_issued;
    return ss.str();
}

} // namespace reportmaker
