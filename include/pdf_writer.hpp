#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Render a markdown report string as a PDF.
// Returns raw PDF bytes on success, empty vector on failure.
std::vector<uint8_t> render_pdf(const std::string& markdown);
