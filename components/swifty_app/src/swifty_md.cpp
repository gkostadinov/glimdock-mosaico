// SPDX-License-Identifier: Apache-2.0
#include "swifty_md.h"

#include <ctype.h>
#include <string.h>

#include <vector>

#include "mui.h"

namespace swifty {
namespace {
using namespace mui;
constexpr int BODY = 16;
constexpr uint32_t CODE_INK = 0xFFD58A, LINK = 0x7DD3FF, QUOTE_BAR = 0x35B2FF, CODE_BG = 0x101418;

struct Style {
    bool bold = false, italic = false, code = false, strike = false, link = false;
};

// Each span needs its own style object that lives as long as the span; they
// are kept on the span group and freed with it.
struct Styles {
    std::vector<lv_style_t *> all;
    ~Styles()
    {
        for (lv_style_t *s : all) {
            lv_style_reset(s);
            delete s;
        }
    }
};

void free_styles(lv_event_t *e)
{
    delete static_cast<Styles *>(lv_event_get_user_data(e));
}

lv_obj_t *spans(lv_obj_t *parent, int width, int px, uint32_t color)
{
    lv_obj_t *g = lv_spangroup_create(parent);
    lv_obj_set_width(g, width);
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_spangroup_set_mode(g, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_font(g, font(px, false), 0);
    lv_obj_set_style_text_color(g, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(g, 3, 0);
    auto *styles = new Styles;
    lv_obj_set_user_data(g, styles);
    lv_obj_add_event_cb(g, free_styles, LV_EVENT_DELETE, styles);
    return g;
}

void add_span(lv_obj_t *g, const std::string &text, const Style &st, int px, uint32_t color)
{
    if (text.empty()) {
        return;
    }
    lv_span_t *s = lv_spangroup_add_span(g);
    lv_spangroup_set_span_text(g, s, text.c_str());
    auto *style = new lv_style_t;
    lv_style_init(style);
    static_cast<Styles *>(lv_obj_get_user_data(g))->all.push_back(style);
    lv_style_set_text_font(style, font(px, st.bold || st.code));
    uint32_t c = color;
    if (st.code) {
        c = CODE_INK;
    } else if (st.link) {
        c = LINK;
    } else if (st.italic) {
        c = TEXT2;
    }
    lv_style_set_text_color(style, lv_color_hex(c));
    int decor = LV_TEXT_DECOR_NONE;
    if (st.strike) {
        decor |= LV_TEXT_DECOR_STRIKETHROUGH;
    }
    if (st.link) {
        decor |= LV_TEXT_DECOR_UNDERLINE;
    }
    lv_style_set_text_decor(style, lv_text_decor_t(decor));
    lv_spangroup_set_span_style(g, s, style);
}

// Inline Markdown into spans.
void inline_md(lv_obj_t *g, const std::string &t, int px, uint32_t color)
{
    Style st;
    std::string run;
    auto flush = [&] {
        add_span(g, run, st, px, color);
        run.clear();
    };
    for (size_t i = 0; i < t.size();) {
        const char c = t[i];
        if (c == '\\' && i + 1 < t.size() && strchr("\\`*_[]()#+-.!~|", t[i + 1])) {
            run += t[i + 1];
            i += 2;
        } else if (c == '`') {
            const size_t end = t.find('`', i + 1);
            if (end == std::string::npos) {
                run += c, ++i;
                continue;
            }
            flush();
            Style code = st;
            code.code = true;
            add_span(g, t.substr(i + 1, end - i - 1), code, px, color);
            i = end + 1;
        } else if ((c == '*' || c == '_') && i + 1 < t.size() && t[i + 1] == c) {
            flush();
            st.bold = !st.bold;
            i += 2;
        } else if (c == '*' || c == '_') {
            // A lone star toggles italic unless it stands alone ("a * b"); an
            // underscore only at a word boundary (snake_case stays as it is).
            const bool prev_word = i > 0 && isalnum((unsigned char)t[i - 1]);
            const bool next_word = i + 1 < t.size() && isalnum((unsigned char)t[i + 1]);
            const bool next_space = i + 1 >= t.size() || t[i + 1] == ' ';
            const bool toggles = c == '*' ? (st.italic || !next_space) : (st.italic ? !next_word : !prev_word && next_word);
            if (!toggles) {
                run += c, ++i;
                continue;
            }
            flush();
            st.italic = !st.italic;
            ++i;
        } else if (c == '~' && i + 1 < t.size() && t[i + 1] == '~') {
            flush();
            st.strike = !st.strike;
            i += 2;
        } else if (c == '[') {
            const size_t close = t.find("](", i);
            const size_t end = close == std::string::npos ? close : t.find(')', close);
            if (end == std::string::npos) {
                run += c, ++i;
                continue;
            }
            flush();
            Style link = st;
            link.link = true;
            add_span(g, t.substr(i + 1, close - i - 1), link, px, color);
            i = end + 1;
        } else {
            run += c, ++i;
        }
    }
    flush();
    lv_spangroup_refresh(g);
}

lv_obj_t *column(lv_obj_t *parent, int width)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_remove_flag(c, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return c;
}

lv_obj_t *row(lv_obj_t *parent, int width)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_remove_flag(r, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return r;
}

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool is_rule(const std::string &t)
{
    if (t.size() < 3) {
        return false;
    }
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_') {
        return false;
    }
    for (char k : t)
        if (k != c && k != ' ') {
            return false;
        }
    return true;
}

// "- item", "* item", "1. item": returns the marker and sets body.
std::string list_marker(const std::string &line, std::string &body, int &indent)
{
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') {
        ++i;
    }
    indent = int(i / 2);
    if (i + 1 < line.size() && strchr("-*+", line[i]) && line[i + 1] == ' ') {
        body = line.substr(i + 2);
        return "\xE2\x80\xA2";
    }
    size_t j = i;
    while (j < line.size() && isdigit((unsigned char)line[j])) {
        ++j;
    }
    if (j > i && j + 1 < line.size() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') {
        body = line.substr(j + 2);
        return line.substr(i, j - i) + ".";
    }
    return "";
}

std::vector<std::string> cells(const std::string &line)
{
    std::vector<std::string> out;
    std::string t = trim(line);
    if (!t.empty() && t[0] == '|') {
        t.erase(0, 1);
    }
    if (!t.empty() && t.back() == '|') {
        t.pop_back();
    }
    size_t start = 0;
    for (size_t i = 0; i <= t.size(); ++i)
        if (i == t.size() || t[i] == '|') {
            out.push_back(trim(t.substr(start, i - start)));
            start = i + 1;
        }
    return out;
}
}  // namespace

void md_render(lv_obj_t *parent, const std::string &text, int width, const ImageLoader &images)
{
    lv_obj_clean(parent);
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
        if (i == text.size() || text[i] == '\n') {
            lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    std::string para;
    auto flush_para = [&] {
        const std::string p = trim(para);
        para.clear();
        if (p.empty()) {
            return;
        }
        inline_md(spans(parent, width, BODY, TEXT), p, BODY, TEXT);
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &raw = lines[i];
        const std::string t = trim(raw);
        if (t.compare(0, 3, "```") == 0) {  // code block, to the closing fence (or the end while streaming)
            flush_para();
            const std::string lang = trim(t.substr(3));  // ```cpp: named above the code
            std::string code;
            for (++i; i < lines.size() && trim(lines[i]).compare(0, 3, "```") != 0; ++i) {
                code += (code.empty() ? "" : "\n") + lines[i];
            }
            // Tabs as four spaces: the label would draw them as one narrow glyph.
            for (size_t at = code.find('\t'); at != std::string::npos; at = code.find('\t', at + 4)) {
                code.replace(at, 1, "    ");
            }
            lv_obj_t *box_ = lv_obj_create(parent);
            lv_obj_remove_style_all(box_);
            lv_obj_set_size(box_, width, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(box_, lv_color_hex(CODE_BG), 0);
            lv_obj_set_style_bg_opa(box_, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(box_, 10, 0);
            lv_obj_set_style_pad_all(box_, 10, 0);
            // A bar down the left, as editors mark a block.
            lv_obj_set_style_border_side(box_, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(box_, 3, 0);
            lv_obj_set_style_border_color(box_, lv_color_hex(CODE_INK), 0);
            lv_obj_set_style_border_opa(box_, LV_OPA_40, 0);
            lv_obj_remove_flag(box_, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            if (!lang.empty() && lang.size() < 16) {
                lv_obj_set_flex_flow(box_, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_style_pad_row(box_, 4, 0);
                lv_obj_t *caption = lv_label_create(box_);
                lv_label_set_text(caption, lang.c_str());
                lv_obj_set_style_text_font(caption, font(11, true), 0);
                lv_obj_set_style_text_color(caption, lv_color_hex(TEXT3), 0);
            }
            lv_obj_t *l = lv_label_create(box_);
            lv_label_set_text(l, code.c_str());
            lv_obj_set_width(l, width - 23);  // inside the padding and the bar
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_font(l, font(13, true), 0);
            lv_obj_set_style_text_color(l, lv_color_hex(CODE_INK), 0);
            continue;
        }
        if (t.empty()) {
            flush_para();
            continue;
        }
        if (t[0] == '#') {
            flush_para();
            size_t level = 0;
            while (level < t.size() && t[level] == '#') {
                ++level;
            }
            const int px = level == 1 ? 22 : level == 2 ? 19 : 17;
            lv_obj_t *g = spans(parent, width, px, TEXT);
            Style bold;
            bold.bold = true;
            add_span(g, trim(t.substr(level)), bold, px, TEXT);
            lv_spangroup_refresh(g);
            continue;
        }
        if (is_rule(t)) {
            flush_para();
            lv_obj_t *r = lv_obj_create(parent);
            lv_obj_remove_style_all(r);
            lv_obj_set_size(r, width, 1);
            lv_obj_set_style_bg_color(r, lv_color_hex(EDGE), 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            continue;
        }
        if (t.size() > 4 && t.compare(0, 2, "![") == 0 && t.back() == ')') {  // an image on its own line
            const size_t close = t.find("](");
            if (close != std::string::npos) {
                flush_para();
                lv_obj_t *holder = lv_obj_create(parent);
                lv_obj_remove_style_all(holder);
                lv_obj_set_size(holder, width, 60);
                lv_obj_set_style_bg_color(holder, lv_color_hex(FIELD), 0);
                lv_obj_set_style_bg_opa(holder, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(holder, 14, 0);
                lv_obj_set_style_clip_corner(holder, true, 0);
                lv_obj_remove_flag(holder, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
                const std::string alt = t.substr(2, close - 2), url = t.substr(close + 2, t.size() - close - 3);
                lv_obj_t *l = label(holder, alt.empty() ? "Loading image\xE2\x80\xA6" : alt.c_str(), 14, 20, width - 28, TEXT3, 14);
                lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
                if (images) {
                    images(url, alt, holder);
                }
                continue;
            }
        }
        if (t[0] == '>') {
            flush_para();
            lv_obj_t *r = row(parent, width);
            lv_obj_t *bar_ = lv_obj_create(r);
            lv_obj_remove_style_all(bar_);
            lv_obj_set_size(bar_, 3, LV_PCT(100));
            lv_obj_set_style_bg_color(bar_, lv_color_hex(QUOTE_BAR), 0);
            lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bar_, 2, 0);
            lv_obj_set_height(r, LV_SIZE_CONTENT);
            inline_md(spans(r, width - 12, BODY, TEXT2), trim(t.substr(1)), BODY, TEXT2);
            continue;
        }
        if (t[0] == '|' && i + 1 < lines.size() && trim(lines[i + 1]).find("---") != std::string::npos) {
            flush_para();
            std::vector<std::vector<std::string>> table{cells(t)};
            for (i += 2; i < lines.size() && !trim(lines[i]).empty() && trim(lines[i])[0] == '|'; ++i) {
                table.push_back(cells(lines[i]));
            }
            --i;
            const size_t cols = std::max<size_t>(1, table[0].size());
            const int cw = (width - int(cols - 1) * 6) / int(cols);
            lv_obj_t *grid = column(parent, width);
            lv_obj_set_style_pad_row(grid, 2, 0);
            for (size_t r = 0; r < table.size(); ++r) {
                lv_obj_t *rr = row(grid, width);
                lv_obj_set_style_pad_column(rr, 6, 0);
                lv_obj_set_style_bg_color(rr, lv_color_hex(r == 0 ? EDGE : FIELD), 0);
                lv_obj_set_style_bg_opa(rr, r == 0 || r % 2 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                lv_obj_set_style_radius(rr, 6, 0);
                for (size_t c = 0; c < cols; ++c) {
                    lv_obj_t *g = spans(rr, cw, 14, TEXT);
                    lv_obj_set_style_pad_all(g, 4, 0);
                    if (r == 0) {
                        Style bold;
                        bold.bold = true;
                        add_span(g, c < table[r].size() ? table[r][c] : "", bold, 14, TEXT);
                        lv_spangroup_refresh(g);
                    } else {
                        inline_md(g, c < table[r].size() ? table[r][c] : "", 14, TEXT);
                    }
                }
            }
            continue;
        }
        std::string body;
        int indent = 0;
        const std::string marker = list_marker(raw, body, indent);
        if (!marker.empty()) {
            flush_para();
            lv_obj_t *r = row(parent, width);
            lv_obj_set_style_pad_left(r, indent * 16, 0);
            const int mw = marker.size() > 3 ? 26 : 16;
            lv_obj_t *m = lv_label_create(r);
            lv_label_set_text(m, marker.c_str());
            lv_obj_set_width(m, mw);
            lv_obj_set_style_text_font(m, font(BODY, true), 0);
            lv_obj_set_style_text_color(m, lv_color_hex(QUOTE_BAR), 0);
            inline_md(spans(r, width - mw - 8 - indent * 16, BODY, TEXT), trim(body), BODY, TEXT);
            continue;
        }
        para += (para.empty() ? "" : " ") + t;
    }
    flush_para();
}

}  // namespace swifty
