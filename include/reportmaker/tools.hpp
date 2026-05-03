#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace reportmaker {

// OpenAI-compatible tool schemas sent to DeepSeek
nlohmann::json tool_schemas();

// Execute a tool by name with the given arguments JSON.
// Returns a plain-text string result for the AI.
std::string execute_tool(const std::string& name, const nlohmann::json& args);

} // namespace reportmaker
