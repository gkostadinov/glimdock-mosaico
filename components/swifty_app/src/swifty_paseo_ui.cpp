// SPDX-License-Identifier: Apache-2.0
#include "swifty_paseo_ui.h"

#include "mui.h"

using namespace mui;

namespace swifty {
namespace {
// Swifty's sky blue (swifty_ui.cpp).
const Accent SKY = {0x7DF3FF, 0x2B5CF2, 0x35B2FF, 0x041428, 0x12305A, 0x060E1C};
// The card sits inside the panel's rounded corners.
constexpr int CX = 44, CY = 84, CW = 392, CH = 312, PAD = 24;

struct Answer {
    void (*fn)(bool yes, void *ctx);
    void *ctx;
};

void tapped(lv_event_t *e, bool yes)
{
    const Answer *a = static_cast<const Answer *>(lv_event_get_user_data(e));
    if (a && a->fn) {
        a->fn(yes, a->ctx);
    }
}
}  // namespace

lv_obj_t *paseo_confirm_card(lv_obj_t *parent, const PaseoAsk &ask, void (*answer)(bool yes, void *ctx), void *ctx)
{
    lv_obj_t *scrim = box(parent, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);  // what is under it stays untouched
    // Freed with the panel.
    Answer *a = new Answer{answer, ctx};
    lv_obj_add_event_cb(scrim, [](lv_event_t *e) {
        delete static_cast<Answer *>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, a);

    lv_obj_t *card = glass(scrim, CX, CY, CW, CH, 30, SKY);
    dot(card, PAD, 27, 10, SKY.glow);
    label(card, "Paseo", PAD + 18, 20, 200, TEXT2, 15, true);
    label(card, "Asks first", CW - PAD - 120, 20, 120, TEXT3, 13, false, LV_TEXT_ALIGN_RIGHT);

    // The title takes one line or two (measured, not laid out); the rest follows it.
    const lv_font_t *big = font(22, true);
    const int line = lv_font_get_line_height(big);
    lv_point_t size;
    lv_text_get_size(&size, ask.title.c_str(), big, 0, 0, CW - 2 * PAD, LV_TEXT_FLAG_NONE);
    const int title_h = size.y > line ? 2 * line : line;
    lv_obj_t *title = label(card, ask.title.c_str(), PAD, 50, CW - 2 * PAD, TEXT, 22, true);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(title, title_h);
    const int sub_y = 50 + title_h + 4;
    label(card, ask.subtitle.c_str(), PAD, sub_y, CW - 2 * PAD, TEXT3, 14);

    // What the agent gets, as it will read it (the start, when long).
    constexpr int BY = CH - 52 - 22;
    const int quote_y = sub_y + 28, quote_h = BY - 16 - quote_y;
    lv_obj_t *quote = box(card, PAD - 6, quote_y, CW - 2 * PAD + 12, quote_h, 16);
    lv_obj_set_style_bg_color(quote, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(quote, LV_OPA_COVER, 0);
    lv_obj_t *detail = label(quote, ask.detail.c_str(), 14, 10, CW - 2 * PAD - 16, TEXT2, 16);
    const int small = lv_font_get_line_height(font(16, false));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(detail, (quote_h - 20) / small * small);

    constexpr int BW = (CW - 2 * PAD - 12) / 2;
    lv_obj_t *cancel = field_button(card, PAD, BY, BW, 52, "Cancel", 17, TEXT);
    lv_obj_add_event_cb(cancel, [](lv_event_t *e) {
        tapped(e, false);
    }, LV_EVENT_CLICKED, a);
    lv_obj_t *go = fill_button(card, PAD + BW + 12, BY, BW, 52, ask.action.c_str(), SKY, 17);
    lv_obj_add_event_cb(go, [](lv_event_t *e) {
        tapped(e, true);
    }, LV_EVENT_CLICKED, a);
    return scrim;
}

}  // namespace swifty
