#pragma once
#include <string>

namespace reportmaker {

struct AgentConfig {
    std::string base_url    = "https://api.deepseek.com";
    std::string api_key;
    std::string model       = "deepseek-chat";
    int         max_iterations = 20;
    int         max_tokens     = 16000;
    bool        use_cache      = true;
    bool        use_context    = true;
    bool        verbose        = true;
    // Budget limits — loaded from config, fall back to defaults if not set
    int         ss_min = 2;
    int         ss_max = 5;
    int         fc_min = 3;
    int         fc_max = 8;
};

// Run the full agentic report loop. Returns the final markdown report.
std::string run_report(const std::string& query, const AgentConfig& cfg);

} // namespace reportmaker
