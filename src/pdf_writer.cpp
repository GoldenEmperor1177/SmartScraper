#include "pdf_writer.hpp"
#include <hpdf.h>
#include <csetjmp>
#include <sstream>
#include <vector>
#include <string>

// ── libharu error handler ────────────────────────────────────────────────────

static jmp_buf g_hpdf_jmp;

static void hpdf_error_cb(HPDF_STATUS ec, HPDF_STATUS dc, void*) {
    (void)ec; (void)dc;
    longjmp(g_hpdf_jmp, 1);
}

// ── Page / layout constants ──────────────────────────────────────────────────

static constexpr float PAGE_W      = 595.0f;   // A4
static constexpr float PAGE_H      = 842.0f;
static constexpr float MARGIN_L    = 60.0f;
static constexpr float MARGIN_R    = 60.0f;
static constexpr float MARGIN_TOP  = 60.0f;
static constexpr float MARGIN_BOT  = 70.0f;
static constexpr float BODY_W      = PAGE_W - MARGIN_L - MARGIN_R;
static constexpr float FONT_BODY   = 11.0f;
static constexpr float FONT_H1     = 20.0f;
static constexpr float FONT_H2     = 16.0f;
static constexpr float FONT_H3     = 13.0f;
static constexpr float FONT_MONO   = 9.0f;
static constexpr float LINE_BODY   = 16.0f;
static constexpr float LINE_H1     = 28.0f;
static constexpr float LINE_H2     = 24.0f;
static constexpr float LINE_H3     = 20.0f;
static constexpr float LINE_MONO   = 13.0f;
static constexpr float LIST_INDENT = 18.0f;

// ── Renderer state ───────────────────────────────────────────────────────────

struct Renderer {
    HPDF_Doc   pdf;
    HPDF_Page  page;
    HPDF_Font  font_reg;
    HPDF_Font  font_bold;
    HPDF_Font  font_italic;
    HPDF_Font  font_mono;
    float      y;

    void new_page() {
        page = HPDF_AddPage(pdf);
        HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_A4, HPDF_PAGE_PORTRAIT);
        y = PAGE_H - MARGIN_TOP;
    }

    void ensure_space(float need) {
        if (y - need < MARGIN_BOT) new_page();
    }

    // Draw a single-line text segment at (x, y) with given font/size
    void draw_text(float x, float cur_y, HPDF_Font f, float size, const std::string& text) {
        if (text.empty()) return;
        HPDF_Page_SetFontAndSize(page, f, size);
        HPDF_Page_BeginText(page);
        HPDF_Page_TextOut(page, x, cur_y, text.c_str());
        HPDF_Page_EndText(page);
    }

    // Word-wrap text into lines that fit within max_width
    std::vector<std::string> wrap(HPDF_Font f, float size, const std::string& text, float max_width) {
        HPDF_Page_SetFontAndSize(page, f, size);
        std::vector<std::string> lines;
        std::istringstream ss(text);
        std::string word, line;
        while (ss >> word) {
            std::string candidate = line.empty() ? word : line + " " + word;
            float w = HPDF_Page_TextWidth(page, candidate.c_str());
            if (w <= max_width) {
                line = candidate;
            } else {
                if (!line.empty()) lines.push_back(line);
                line = word;
            }
        }
        if (!line.empty()) lines.push_back(line);
        if (lines.empty()) lines.push_back("");
        return lines;
    }

    // Render inline bold/italic spans within a paragraph line
    // Segments: plain text, **bold**, *italic*
    void render_inline(float x, float cur_y, float /*max_width*/, const std::string& text) {
        float cx = x;
        size_t i = 0;
        while (i < text.size()) {
            // Bold: **...**
            if (i + 1 < text.size() && text[i] == '*' && text[i+1] == '*') {
                size_t end = text.find("**", i + 2);
                if (end != std::string::npos) {
                    std::string span = text.substr(i + 2, end - i - 2);
                    HPDF_Page_SetFontAndSize(page, font_bold, FONT_BODY);
                    float w = HPDF_Page_TextWidth(page, span.c_str());
                    HPDF_Page_BeginText(page);
                    HPDF_Page_TextOut(page, cx, cur_y, span.c_str());
                    HPDF_Page_EndText(page);
                    cx += w;
                    i = end + 2;
                    continue;
                }
            }
            // Italic: *...*
            if (text[i] == '*') {
                size_t end = text.find('*', i + 1);
                if (end != std::string::npos) {
                    std::string span = text.substr(i + 1, end - i - 1);
                    HPDF_Page_SetFontAndSize(page, font_italic, FONT_BODY);
                    float w = HPDF_Page_TextWidth(page, span.c_str());
                    HPDF_Page_BeginText(page);
                    HPDF_Page_TextOut(page, cx, cur_y, span.c_str());
                    HPDF_Page_EndText(page);
                    cx += w;
                    i = end + 1;
                    continue;
                }
            }
            // Plain text — accumulate until next marker
            size_t next = text.find('*', i);
            std::string plain = text.substr(i, next == std::string::npos ? std::string::npos : next - i);
            HPDF_Page_SetFontAndSize(page, font_reg, FONT_BODY);
            HPDF_Page_BeginText(page);
            HPDF_Page_TextOut(page, cx, cur_y, plain.c_str());
            HPDF_Page_EndText(page);
            float w = HPDF_Page_TextWidth(page, plain.c_str());
            cx += w;
            i = (next == std::string::npos) ? text.size() : next;
        }
    }

    void render_paragraph(const std::string& text, float x, float avail_w, float line_h, HPDF_Font f, float size) {
        auto lines = wrap(f, size, text, avail_w);
        for (auto& ln : lines) {
            ensure_space(line_h);
            if (f == font_reg && text.find('*') != std::string::npos) {
                render_inline(x, y, avail_w, ln);
            } else {
                draw_text(x, y, f, size, ln);
            }
            y -= line_h;
        }
    }

    void blank(float gap = 6.0f) { y -= gap; }
};

// ── Line classification ───────────────────────────────────────────────────────

enum class LineType { H1, H2, H3, LIST, HR, TABLE, BLANK, PARA };

static LineType classify(const std::string& line) {
    if (line.empty()) return LineType::BLANK;
    if (line.substr(0, 4) == "### ") return LineType::H3;
    if (line.substr(0, 3) == "## ") return LineType::H2;
    if (line.substr(0, 2) == "# ") return LineType::H1;
    if (line.size() >= 3 && (line.substr(0,3) == "---" || line.substr(0,3) == "===")) return LineType::HR;
    if (!line.empty() && line[0] == '|') return LineType::TABLE;
    if (line.size() >= 2 && (line.substr(0,2) == "- " || line.substr(0,2) == "* ")) return LineType::LIST;
    return LineType::PARA;
}

static std::string strip_prefix(const std::string& line, size_t n) {
    return line.size() > n ? line.substr(n) : "";
}

// ── Main render ───────────────────────────────────────────────────────────────

std::vector<uint8_t> render_pdf(const std::string& markdown) {
    HPDF_Doc pdf = HPDF_New(hpdf_error_cb, nullptr);
    if (!pdf) return {};

    if (setjmp(g_hpdf_jmp)) {
        HPDF_Free(pdf);
        return {};
    }

    HPDF_SetCompressionMode(pdf, HPDF_COMP_ALL);

    Renderer r;
    r.pdf       = pdf;
    r.font_reg  = HPDF_GetFont(pdf, "Helvetica",        nullptr);
    r.font_bold = HPDF_GetFont(pdf, "Helvetica-Bold",   nullptr);
    r.font_italic = HPDF_GetFont(pdf, "Helvetica-Oblique", nullptr);
    r.font_mono = HPDF_GetFont(pdf, "Courier",          nullptr);
    r.new_page();

    std::istringstream ss(markdown);
    std::string line;
    int blank_run = 0;

    while (std::getline(ss, line)) {
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        LineType lt = classify(line);

        if (lt == LineType::BLANK) {
            blank_run++;
            if (blank_run == 1) r.blank(8.0f);
            continue;
        }
        blank_run = 0;

        switch (lt) {
            case LineType::H1: {
                r.ensure_space(LINE_H1 + 6);
                r.blank(6.0f);
                std::string txt = strip_prefix(line, 2);
                r.draw_text(MARGIN_L, r.y, r.font_bold, FONT_H1, txt);
                r.y -= LINE_H1;
                // underline
                HPDF_Page_SetLineWidth(r.page, 1.0f);
                HPDF_Page_MoveTo(r.page, MARGIN_L, r.y + 4);
                HPDF_Page_LineTo(r.page, PAGE_W - MARGIN_R, r.y + 4);
                HPDF_Page_Stroke(r.page);
                r.blank(4.0f);
                break;
            }
            case LineType::H2: {
                r.ensure_space(LINE_H2 + 6);
                r.blank(8.0f);
                std::string txt = strip_prefix(line, 3);
                r.draw_text(MARGIN_L, r.y, r.font_bold, FONT_H2, txt);
                r.y -= LINE_H2;
                HPDF_Page_SetLineWidth(r.page, 0.5f);
                HPDF_Page_MoveTo(r.page, MARGIN_L, r.y + 3);
                HPDF_Page_LineTo(r.page, PAGE_W - MARGIN_R, r.y + 3);
                HPDF_Page_Stroke(r.page);
                r.blank(4.0f);
                break;
            }
            case LineType::H3: {
                r.ensure_space(LINE_H3 + 4);
                r.blank(6.0f);
                std::string txt = strip_prefix(line, 4);
                r.draw_text(MARGIN_L, r.y, r.font_bold, FONT_H3, txt);
                r.y -= LINE_H3;
                r.blank(2.0f);
                break;
            }
            case LineType::LIST: {
                std::string txt = strip_prefix(line, 2);
                float avail = BODY_W - LIST_INDENT;
                auto wrapped = r.wrap(r.font_reg, FONT_BODY, txt, avail);
                for (size_t wi = 0; wi < wrapped.size(); wi++) {
                    r.ensure_space(LINE_BODY);
                    if (wi == 0) {
                        r.draw_text(MARGIN_L, r.y, r.font_reg, FONT_BODY, "\xe2\x80\xa2"); // UTF-8 bullet
                        r.render_inline(MARGIN_L + LIST_INDENT, r.y, avail, wrapped[wi]);
                    } else {
                        r.render_inline(MARGIN_L + LIST_INDENT, r.y, avail, wrapped[wi]);
                    }
                    r.y -= LINE_BODY;
                }
                break;
            }
            case LineType::HR: {
                r.blank(4.0f);
                HPDF_Page_SetLineWidth(r.page, 0.5f);
                HPDF_Page_MoveTo(r.page, MARGIN_L, r.y);
                HPDF_Page_LineTo(r.page, PAGE_W - MARGIN_R, r.y);
                HPDF_Page_Stroke(r.page);
                r.blank(4.0f);
                break;
            }
            case LineType::TABLE: {
                r.ensure_space(LINE_MONO);
                // Strip leading/trailing | and render as monospace
                std::string txt = line;
                if (!txt.empty() && txt.front() == '|') txt = txt.substr(1);
                if (!txt.empty() && txt.back() == '|') txt.pop_back();
                // Skip separator rows (---|---)
                bool is_sep = true;
                for (char c : txt) if (c != '-' && c != '|' && c != ' ' && c != ':') { is_sep = false; break; }
                if (!is_sep) {
                    r.draw_text(MARGIN_L, r.y, r.font_mono, FONT_MONO, txt);
                    r.y -= LINE_MONO;
                }
                break;
            }
            case LineType::PARA: {
                r.render_paragraph(line, MARGIN_L, BODY_W, LINE_BODY, r.font_reg, FONT_BODY);
                break;
            }
            default: break;
        }
    }

    // Write to in-memory stream
    HPDF_SaveToStream(pdf);
    HPDF_UINT32 size = HPDF_GetStreamSize(pdf);
    HPDF_ResetStream(pdf);
    std::vector<uint8_t> buf(size);
    HPDF_UINT32 read_size = size;
    HPDF_ReadFromStream(pdf, buf.data(), &read_size);
    HPDF_Free(pdf);
    return buf;
}
