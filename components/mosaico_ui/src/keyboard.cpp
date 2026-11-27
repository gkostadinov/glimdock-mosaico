// SPDX-License-Identifier: Apache-2.0
// An iOS-style keyboard for the round-cornered 480x480 panel, in the
// ESP-Mosaico's own keyboard colours (ESP-Claw's settings keyboard: #55555A
// letter keys, #3B3C3D function keys, #ECECF0 labels, 8 px corners, an accent
// action key). Letters, 123 and #+= layouts; a magnified bubble over the key
// being pressed; one-shot shift, double-tap caps lock, automatic capitals;
// delete repeats while held; a double space types ". ". Built once: changing
// layout or case only relabels the keys.
#include <ctype.h>
#include <string.h>

#include <string>

#include "home_icons.h"
#include "mui.h"

namespace mui {
namespace {
constexpr uint32_t KEY = 0x55555A, FN = 0x3B3C3D, KEY_TEXT = 0xECECF0, PRESSED = 0x7A7A80;
constexpr int ROWS = 4, LETTERS = 26;
constexpr int GAP_X = 6, GAP_Y = 9;

// Line icons on the 24 grid, drawn with draw_icon_mask.
constexpr float shift_0[] = {12, 3.5f, 3.5f, 12, 8.5f, 12, 8.5f, 19.5f, 15.5f, 19.5f, 15.5f, 12, 20.5f, 12, 12, 3.5f};
constexpr home_icons::Path shift_paths[] = {{shift_0, 8}};
constexpr float lock_1[] = {8.5f, 22, 15.5f, 22};
constexpr home_icons::Path lock_paths[] = {{shift_0, 8}, {lock_1, 2}};
constexpr float del_0[] = {8.5f, 5.5f, 21, 5.5f, 21, 18.5f, 8.5f, 18.5f, 2.5f, 12, 8.5f, 5.5f};
constexpr float del_1[] = {11.5f, 9, 17.5f, 15}, del_2[] = {17.5f, 9, 11.5f, 15};
constexpr home_icons::Path del_paths[] = {{del_0, 6}, {del_1, 2}, {del_2, 2}};
constexpr home_icons::Icon SHIFT = {"kb-shift", shift_paths, 1}, LOCK = {"kb-lock", lock_paths, 2},
          DELETE = {"kb-delete", del_paths, 3};

const char *const LOWER[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
const char *const NUM[3][10] = {{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"-", "/", ":", ";", "(", ")", "$", "&", "@", "\""},
    {".", ",", "?", "!", "'", nullptr}
};
const char *const SYM[3][10] = {{"[", "]", "{", "}", "#", "%", "^", "*", "+", "="},
    {"_", "\\", "|", "~", "<", ">", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x80\xA2"},
    {".", ",", "?", "!", "'", nullptr}
};
void (*haptic_tick)() = nullptr;

enum class Layout : uint8_t { Letters, Numbers, Symbols };
enum class Shift : uint8_t { Off, Once, Lock };

struct State {
    lv_obj_t *kb = nullptr, *ta = nullptr, *bubble = nullptr, *bubble_label = nullptr;
    lv_obj_t *rows[3][10] = {};  // character keys, by row
    lv_obj_t *shift = nullptr, *del = nullptr, *mode = nullptr, *alt = nullptr, *space = nullptr, *action = nullptr;
    KeyboardConfig cfg;
    Layout layout = Layout::Letters;
    Shift shift_state = Shift::Off;
    uint32_t last_shift = 0, last_space = 0;
    int w = 0, kw = 0, kh = 0, row3_inner = 0;  // geometry, for re-centring rows
};

State *state_of(lv_obj_t *o)
{
    while (o && !lv_obj_get_user_data(o)) {
        o = lv_obj_get_parent(o);
    }
    return o ? static_cast<State *>(lv_obj_get_user_data(o)) : nullptr;
}

lv_obj_t *key_label(lv_obj_t *k)
{
    return lv_obj_get_child(k, 0);
}

const char *key_text(State *s, int r, int c)
{
    static char one[2];
    if (s->layout == Layout::Letters) {
        if (c >= int(strlen(LOWER[r]))) {
            return nullptr;
        }
        const bool up = s->cfg.caps_only || s->shift_state != Shift::Off;
        one[0] = char(up ? toupper(LOWER[r][c]) : LOWER[r][c]);
        one[1] = 0;
        return one;
    }
    const char *const(*t)[10] = s->layout == Layout::Numbers ? NUM : SYM;
    return t[r][c];
}

void paint(lv_obj_t *k, uint32_t bg, uint32_t fg)
{
    lv_obj_set_style_bg_color(k, lv_color_hex(bg), 0);
    lv_obj_set_style_line_color(k, lv_color_hex(fg), 0);
    if (lv_obj_get_child_count(k)) {
        lv_obj_set_style_text_color(key_label(k), lv_color_hex(fg), 0);
    }
}

// Centres the visible keys of a row; row 3 sits between shift and delete and
// its keys widen to fill when there are fewer (5 punctuation keys vs 7 letters).
void reflow(State *s, int r)
{
    int n = 0;
    for (int c = 0; c < 10; ++c)
        if (s->rows[r][c] && !lv_obj_has_flag(s->rows[r][c], LV_OBJ_FLAG_HIDDEN)) {
            ++n;
        }
    if (!n) {
        return;
    }
    int kw = s->kw;
    if (r == 2) {
        kw = std::min((s->row3_inner - (n - 1) * GAP_X) / n, n < 7 ? s->kw * 3 / 2 : s->kw);
    }
    const int row_w = n * kw + (n - 1) * GAP_X;
    int x = (s->w - row_w) / 2;
    for (int c = 0; c < 10; ++c) {
        lv_obj_t *k = s->rows[r][c];
        if (!k || lv_obj_has_flag(k, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_obj_set_x(k, x);
        lv_obj_set_width(k, kw);
        x += kw + GAP_X;
    }
}

// Relabels everything for the current layout and shift.
void refresh(State *s)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 10; ++c) {
            lv_obj_t *k = s->rows[r][c];
            if (!k) {
                continue;
            }
            const char *t = key_text(s, r, c);
            if (t) {
                lv_label_set_text(key_label(k), t);
                lv_obj_remove_flag(k, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(k, LV_OBJ_FLAG_HIDDEN);
            }
        }
    for (int r = 1; r < 3; ++r) {
        reflow(s, r);
    }
    const bool letters = s->layout == Layout::Letters;
    // Row 3 on the number pages: "#+=" or "123" where shift sits.
    lv_obj_set_user_data(s->shift, nullptr);
    if (letters) {
        lv_label_set_text(key_label(s->shift), "");
        paint(s->shift, s->shift_state == Shift::Off ? FN : 0xFFFFFF, s->shift_state == Shift::Off ? KEY_TEXT : 0x1C1C1E);
    } else {
        lv_label_set_text(key_label(s->shift), s->layout == Layout::Numbers ? "#+=" : "123");
        paint(s->shift, FN, KEY_TEXT);
    }
    lv_obj_invalidate(s->shift);
    lv_label_set_text(key_label(s->mode), letters ? "123" : "ABC");
}

void tick()
{
    if (haptic_tick) {
        haptic_tick();
    }
}

void type(State *s, const char *text)
{
    if (!s->ta) {
        return;
    }
    lv_textarea_add_text(s->ta, text);
    if (s->layout == Layout::Letters && s->shift_state == Shift::Once) {
        s->shift_state = Shift::Off;
        refresh(s);
    }
}

// Automatic capitals: at the start, and after ". ", "! " or "? ".
void auto_caps(State *s)
{
    if (!s->cfg.auto_caps || s->cfg.caps_only || s->layout != Layout::Letters || s->shift_state == Shift::Lock) {
        return;
    }
    const char *t = s->ta ? lv_textarea_get_text(s->ta) : "";
    const size_t n = strlen(t);
    const bool start = n == 0 || (n >= 2 && t[n - 1] == ' ' && strchr(".!?", t[n - 2]));
    const Shift want = start ? Shift::Once : Shift::Off;
    if (want != s->shift_state) {
        s->shift_state = want;
        refresh(s);
    }
}

void show_bubble(State *s, lv_obj_t *k, const char *text)
{
    lv_area_t a;
    lv_obj_get_coords(k, &a);  // on the screen, as the top layer's are
    const int w = lv_area_get_width(&a) + 16, h = lv_area_get_height(&a) + 30;
    lv_obj_set_size(s->bubble, w, h);
    lv_obj_set_pos(s->bubble, a.x1 - 8, a.y1 - h + lv_area_get_height(&a) - 4 - 30);
    lv_label_set_text(s->bubble_label, text);
    lv_obj_remove_flag(s->bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s->bubble);
}

void char_cb(lv_event_t *e)
{
    lv_obj_t *k = lv_event_get_current_target_obj(e);
    State *s = state_of(lv_obj_get_parent(k));
    const char *text = lv_label_get_text(key_label(k));
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        lv_obj_set_style_bg_color(k, lv_color_hex(PRESSED), 0);
        show_bubble(s, k, text);
        tick();
        break;
    case LV_EVENT_PRESS_LOST:
        lv_obj_set_style_bg_color(k, lv_color_hex(KEY), 0);
        lv_obj_add_flag(s->bubble, LV_OBJ_FLAG_HIDDEN);
        break;
    case LV_EVENT_RELEASED: {
        lv_obj_set_style_bg_color(k, lv_color_hex(KEY), 0);
        lv_obj_add_flag(s->bubble, LV_OBJ_FLAG_HIDDEN);
        const std::string t = text;
        type(s, t.c_str());
        // after punctuation on the number pages, back to letters (as iOS does with "'")
        if (s->layout != Layout::Letters && t == "'") {
            s->layout = Layout::Letters;
            refresh(s);
        }
        auto_caps(s);
        break;
    }
    default:
        break;
    }
}

void fn_press(lv_event_t *e)
{
    lv_obj_t *k = lv_event_get_current_target_obj(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        lv_obj_set_style_opa(k, 170, 0);
        tick();
    } else if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_opa(k, LV_OPA_COVER, 0);
    }
}

void shift_cb(lv_event_t *e)
{
    State *s = state_of(lv_obj_get_parent(lv_event_get_current_target_obj(e)));
    if (s->layout != Layout::Letters) {  // "#+=" / "123" on the number pages
        s->layout = s->layout == Layout::Numbers ? Layout::Symbols : Layout::Numbers;
        refresh(s);
        return;
    }
    if (s->cfg.caps_only) {
        return;
    }
    const uint32_t now = lv_tick_get();
    if (s->shift_state == Shift::Once && now - s->last_shift < 350) {
        s->shift_state = Shift::Lock;    // double tap
    } else {
        s->shift_state = s->shift_state == Shift::Off ? Shift::Once : Shift::Off;
    }
    s->last_shift = now;
    refresh(s);
}

void delete_cb(lv_event_t *e)
{
    State *s = state_of(lv_obj_get_parent(lv_event_get_current_target_obj(e)));
    if (!s->ta) {
        return;
    }
    lv_textarea_delete_char(s->ta);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED_REPEAT) {
        tick();
    }
    auto_caps(s);
}

void mode_cb(lv_event_t *e)
{
    State *s = state_of(lv_obj_get_parent(lv_event_get_current_target_obj(e)));
    s->layout = s->layout == Layout::Letters ? Layout::Numbers : Layout::Letters;
    refresh(s);
    auto_caps(s);
}

void space_cb(lv_event_t *e)
{
    State *s = state_of(lv_obj_get_parent(lv_event_get_current_target_obj(e)));
    if (!s->ta) {
        return;
    }
    const char *t = lv_textarea_get_text(s->ta);
    const size_t n = strlen(t);
    const uint32_t now = lv_tick_get();
    // double space: ". " after a word
    if (s->cfg.auto_caps && n >= 2 && t[n - 1] == ' ' && isalnum((unsigned char)t[n - 2]) && now - s->last_space < 450) {
        lv_textarea_delete_char(s->ta);
        lv_textarea_add_text(s->ta, ". ");
    } else {
        lv_textarea_add_text(s->ta, " ");
    }
    s->last_space = now;
    if (s->layout != Layout::Letters) {
        s->layout = Layout::Letters;
        refresh(s);
    }
    auto_caps(s);
}

void action_cb(lv_event_t *e)
{
    State *s = state_of(lv_obj_get_parent(lv_event_get_current_target_obj(e)));
    if (s->ta) {
        lv_obj_send_event(s->ta, LV_EVENT_READY, nullptr);
    }
}

void draw_glyph(lv_event_t *e)
{
    lv_obj_t *k = lv_event_get_current_target_obj(e);
    auto *icon = static_cast<const home_icons::Icon *>(lv_event_get_user_data(e));
    State *s = state_of(lv_obj_get_parent(k));
    if (icon == &SHIFT && s && s->layout != Layout::Letters) {
        return;    // shows "#+=" / "123" instead
    }
    if (icon == &SHIFT && s && s->shift_state == Shift::Lock) {
        icon = &LOCK;
    }
    lv_area_t a;
    lv_obj_get_coords(k, &a);
    const int size = 26, x = a.x1 + (lv_area_get_width(&a) - size) / 2, y = a.y1 + (lv_area_get_height(&a) - size) / 2;
    const lv_area_t area = {x, y, x + size - 1, y + size - 1};
    draw_icon_mask(lv_event_get_layer(e), area, icon, lv_obj_get_style_line_color(k, LV_PART_MAIN), 2);
}

lv_obj_t *make_key(State *s, int x, int y, int w, int h, uint32_t bg, const char *text, int px, bool semi)
{
    lv_obj_t *k = box(s->kb, x, y, w, h, 8);
    lv_obj_add_flag(k, LV_OBJ_FLAG_USER_1);  // ticks on press itself: no tap pulse from the shell
    lv_obj_set_style_bg_color(k, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_line_color(k, lv_color_hex(KEY_TEXT), 0);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(k);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font(px, semi), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(KEY_TEXT), 0);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return k;
}

void free_state(lv_event_t *e)
{
    auto *s = static_cast<State *>(lv_event_get_user_data(e));
    if (s->bubble && lv_obj_is_valid(s->bubble)) {
        lv_obj_delete(s->bubble);  // on the top layer, not deleted with the keyboard
    }
    delete s;
}
}  // namespace

void keyboard_set_haptic(void (*tick)())
{
    haptic_tick = tick;
}

lv_obj_t *keyboard(lv_obj_t *parent, lv_obj_t *textarea, int x, int y, int w, int h, const KeyboardConfig &cfg)
{
    auto *s = new State;
    s->cfg = cfg;
    s->ta = textarea;
    s->kb = box(parent, x, y, w, h);
    lv_obj_remove_flag(s->kb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(s->kb, s);
    lv_obj_add_event_cb(s->kb, free_state, LV_EVENT_DELETE, s);
    const int side = cfg.side_inset, bottom_inset = cfg.bottom_inset;
    const int kh = (h - (ROWS - 1) * GAP_Y) / ROWS;
    const int kw = (w - 2 * side - 9 * GAP_X) / 10;
    s->w = w, s->kw = kw, s->kh = kh;
    // rows 1-2: characters, the second one half a key in
    for (int r = 0; r < 2; ++r) {
        const int n = 10;  // the second row shows 9 letters; refresh() re-centres it
        const int row_w = n * kw + (n - 1) * GAP_X;
        int kx = (w - row_w) / 2;
        for (int c = 0; c < n; ++c) {
            s->rows[r][c] = make_key(s, kx, r * (kh + GAP_Y), kw, kh, KEY, "", 21, false);
            for (lv_event_code_t code : {
                        LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST
                    })
                lv_obj_add_event_cb(s->rows[r][c], char_cb, code, nullptr);
            kx += kw + GAP_X;
        }
    }
    // row 3: shift, 7 letters (5 punctuation keys on the number pages), delete
    const int y3 = 2 * (kh + GAP_Y);
    const int fw = kw + kw / 2 + 2;
    s->shift = make_key(s, side, y3, fw, kh, FN, "", 15, true);
    lv_obj_add_event_cb(s->shift, draw_glyph, LV_EVENT_DRAW_MAIN_END, const_cast<home_icons::Icon *>(&SHIFT));
    lv_obj_add_event_cb(s->shift, shift_cb, LV_EVENT_CLICKED, nullptr);
    s->del = make_key(s, w - side - fw, y3, fw, kh, FN, "", 15, true);
    lv_obj_add_event_cb(s->del, draw_glyph, LV_EVENT_DRAW_MAIN_END, const_cast<home_icons::Icon *>(&DELETE));
    for (lv_event_code_t code : {
                LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED_REPEAT
            })
        lv_obj_add_event_cb(s->del, delete_cb, code, nullptr);
    {
        const int inner = w - 2 * side - 2 * fw - 2 * GAP_X - 2 * 4;
        s->row3_inner = inner;
        const int n = 7, w3 = std::min(kw, (inner - (n - 1) * GAP_X) / n);
        const int row_w = n * w3 + (n - 1) * GAP_X;
        int kx = (w - row_w) / 2;
        for (int c = 0; c < n; ++c) {
            s->rows[2][c] = make_key(s, kx, y3, w3, kh, KEY, "", 21, false);
            for (lv_event_code_t code : {
                        LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST
                    })
                lv_obj_add_event_cb(s->rows[2][c], char_cb, code, nullptr);
            kx += w3 + GAP_X;
        }
    }
    // row 4: 123, space, action; kept clear of the rounded bottom corners
    const int y4 = 3 * (kh + GAP_Y), inset = side + bottom_inset;
    const int mw = kw + kw / 2 + 8, aw = std::max(84, text_width(cfg.action, 15, true) + 28);
    s->mode = make_key(s, inset, y4, mw, kh, FN, "123", 15, true);
    lv_obj_add_event_cb(s->mode, mode_cb, LV_EVENT_CLICKED, nullptr);
    s->action = make_key(s, w - inset - aw, y4, aw, kh, cfg.accent->glow, cfg.action, 15, true);
    lv_obj_set_style_text_color(key_label(s->action), lv_color_hex(cfg.accent->ink), 0);
    lv_obj_add_event_cb(s->action, action_cb, LV_EVENT_CLICKED, nullptr);
    const int sx = inset + mw + GAP_X, sw = w - inset - aw - GAP_X - sx;
    s->space = make_key(s, sx, y4, sw, kh, KEY, "space", 15, false);
    lv_obj_add_event_cb(s->space, space_cb, LV_EVENT_CLICKED, nullptr);
    for (lv_obj_t *k : {
                s->shift, s->del, s->mode, s->action, s->space
            })
        for (lv_event_code_t code : {
                    LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST
                })
            lv_obj_add_event_cb(k, fn_press, code, nullptr);
    // The magnified key, over the key being pressed. On the top layer: as the
    // keyboard's child, what its app drew above the keyboard (the field's card)
    // covered the top row's bubble.
    s->bubble = box(lv_layer_top(), 0, 0, 10, 10, 12);
    lv_obj_set_style_bg_color(s->bubble, lv_color_hex(0x6A6A70), 0);
    lv_obj_set_style_bg_opa(s->bubble, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s->bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s->bubble, lv_obj_flag_t(LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT));
    lv_obj_set_style_clip_corner(s->bubble, false, 0);
    s->bubble_label = lv_label_create(s->bubble);
    lv_obj_set_style_text_font(s->bubble_label, font(30, false), 0);
    lv_obj_set_style_text_color(s->bubble_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s->bubble_label, LV_ALIGN_TOP_MID, 0, 6);
    refresh(s);
    auto_caps(s);
    return s->kb;
}

}  // namespace mui
