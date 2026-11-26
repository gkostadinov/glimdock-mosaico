// SPDX-License-Identifier: Apache-2.0
#include "mui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

#include "home_icons.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#ifdef ESP_PLATFORM
extern const uint8_t inter_regular_ttf_start[] asm("_binary_Inter_Regular_ttf_start");
extern const uint8_t inter_regular_ttf_end[] asm("_binary_Inter_Regular_ttf_end");
extern const uint8_t inter_semibold_ttf_start[] asm("_binary_Inter_SemiBold_ttf_start");
extern const uint8_t inter_semibold_ttf_end[] asm("_binary_Inter_SemiBold_ttf_end");
#else
extern "C" const uint8_t inter_regular_ttf[], inter_semibold_ttf[];
extern "C" const size_t inter_regular_ttf_size, inter_semibold_ttf_size;
#define inter_regular_ttf_start inter_regular_ttf
#define inter_regular_ttf_end (inter_regular_ttf + inter_regular_ttf_size)
#define inter_semibold_ttf_start inter_semibold_ttf
#define inter_semibold_ttf_end (inter_semibold_ttf + inter_semibold_ttf_size)
#endif

namespace mui {

const Accent VIOLET = {0x8C73FF, 0x4A2DB0, 0x7A5BF5, 0xFFFFFF, 0x2E224E, 0x100C1C};
const Accent CYAN = {0x6FE3F7, 0x1E8FB0, 0x54D9F2, 0x041820, 0x163A46, 0x081016};
const Accent AMBER = {0xF7CE7E, 0xB07A1E, 0xF0B858, 0x2A1A05, 0x3C2C14, 0x140E06};
const Accent GREEN = {0x8EE6B5, 0x2E7D4F, 0x7AD6A0, 0x06200F, 0x163626, 0x08140C};

namespace {
lv_color_t hex(uint32_t c)
{
    return lv_color_hex(c);
}

template <typename T>
T *state_of(lv_obj_t *o)
{
    return static_cast<T *>(lv_obj_get_user_data(o));
}

template <typename T>
void delete_state(lv_event_t *e)
{
    delete static_cast<T *>(lv_event_get_user_data(e));
}

const lv_font_t *symbols(int px)
{
    if (px >= 40) {
        return &lv_font_montserrat_48;
    }
    if (px >= 28) {
        return &lv_font_montserrat_32;
    }
    if (px >= 22) {
        return &lv_font_montserrat_24;
    }
    if (px >= 18) {
        return &lv_font_montserrat_20;
    }
    if (px >= 15) {
        return &lv_font_montserrat_16;
    }
    if (px >= 13) {
        return &lv_font_montserrat_14;
    }
    return &lv_font_montserrat_12;
}
}  // namespace

// ---- type ------------------------------------------------------------------
const lv_font_t *font(int px, bool semi)
{
    static std::map<int, lv_font_t *> cache;
    px = std::max(6, px);
    const int key = px * 2 + (semi ? 1 : 0);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    const uint8_t *start = semi ? inter_semibold_ttf_start : inter_regular_ttf_start;
    const uint8_t *end = semi ? inter_semibold_ttf_end : inter_regular_ttf_end;
    lv_font_t *f = lv_tiny_ttf_create_data_ex(start, size_t(end - start), px, LV_FONT_KERNING_NORMAL, px >= 40 ? 64 : 384);  // glyphs kept rendered: a miss redraws from the outline
    if (!f) {
        return symbols(px);
    }
    f->fallback = symbols(px);
    cache[key] = f;
    return f;
}

int text_width(const char *text, int px, bool semi)
{
    lv_point_t p;
    lv_text_get_size(&p, text, font(px, semi), px >= 30 ? -1 : 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

// ---- backdrop --------------------------------------------------------------
// Flat, as Vibe Mode: the panel stays true black. Kept as an object so the
// apps can still show or hide "their" background.
lv_obj_t *backdrop(lv_obj_t *parent, const Backdrop &)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(o, 1, 1);
    return o;
}

// ---- primitives -------------------------------------------------------------
lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, std::max(1, w), std::max(1, h));
    lv_obj_remove_flag(o, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w, uint32_t color, int px, bool semi,
                lv_text_align_t align)
{
    // The text comes last: a label measures its text again on every style
    // and size change (six or more times here).
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text_static(l, "");
    lv_obj_set_pos(l, x, y);
    const lv_font_t *f = font(px, semi);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    if (px >= 30) {
        lv_obj_set_style_text_letter_space(l, -1, 0);
    }
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(l, lv_font_get_line_height(f));
    }
    lv_label_set_text(l, text);
    return l;
}

void wrap_label(lv_obj_t *l)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(l, LV_SIZE_CONTENT);
}

void lit_edge(lv_obj_t *) {}

// Vibe Mode surfaces: solid near-black card with a hairline edge.
void style_glass(lv_obj_t *o, const Accent &, lv_opa_t)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, hex(CARD), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_main_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, hex(EDGE), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_TRANSP, 0);
}

void clear_glass(lv_obj_t *o)
{
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_main_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_transform_width(o, 0, 0);
    lv_obj_set_style_transform_height(o, 0, 0);
}

lv_obj_t *glass(lv_obj_t *parent, int x, int y, int w, int h, int radius, const Accent &a)
{
    lv_obj_t *o = box(parent, x, y, w, h, radius);
    style_glass(o, a);
    return o;
}

// The accent as one solid colour (no gradient, no glow).
void style_fill(lv_obj_t *o, const Accent &a, bool)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, hex(a.glow), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_TRANSP, 0);
}

void press_fx(lv_obj_t *o)
{
    static bool ready = false;
    static lv_style_t normal_small, pressed_small, normal_big, pressed_big;
    static lv_style_transition_dsc_t back, in;
    static const lv_style_prop_t props[] = {LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, LV_STYLE_PROP_INV};
    if (!ready) {
        ready = true;
        lv_style_transition_dsc_init(&back, props, lv_anim_path_overshoot, 220, 0, nullptr);
        lv_style_transition_dsc_init(&in, props, lv_anim_path_ease_out, 60, 0, nullptr);
        for (lv_style_t *s : {
                    &normal_small, &pressed_small, &normal_big, &pressed_big
                }) lv_style_init(s);
        lv_style_set_transition(&normal_small, &back);
        lv_style_set_transition(&normal_big, &back);
        lv_style_set_transition(&pressed_small, &in);
        lv_style_set_transition(&pressed_big, &in);
        lv_style_set_transform_width(&pressed_small, -3);
        lv_style_set_transform_height(&pressed_small, -2);
        lv_style_set_transform_width(&pressed_big, -8);
        lv_style_set_transform_height(&pressed_big, -5);
    }
    // The width as set (buttons and cards have fixed sizes): updating the
    // layout to measure it laid the whole screen out again for every button,
    // ~6 ms each on a Glimdock page (most of a 56 ms rebuild).
    int32_t w = lv_obj_get_style_width(o, LV_PART_MAIN);
    if (!LV_COORD_IS_PX(w)) {
        lv_obj_update_layout(o);
        w = lv_obj_get_width(o);
    }
    const bool big = w > 200;
    lv_obj_add_style(o, big ? &normal_big : &normal_small, 0);
    lv_obj_add_style(o, big ? &pressed_big : &pressed_small, LV_STATE_PRESSED);
}

lv_obj_t *fill_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, const Accent &a, int px)
{
    lv_obj_t *b = box(parent, x, y, w, h, LV_RADIUS_CIRCLE);
    style_fill(b, a, true);
    const lv_font_t *f = font(px, true);
    label(b, text, 0, (h - lv_font_get_line_height(f)) / 2, w, a.ink, px, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    return b;
}

lv_obj_t *field_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int px, uint32_t color)
{
    lv_obj_t *b = box(parent, x, y, w, h, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(b, hex(FIELD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, hex(EDGE), LV_STATE_PRESSED);
    if (text && *text) {
        const lv_font_t *f = font(px, true);
        label(b, text, 0, (h - lv_font_get_line_height(f)) / 2, w, color, px, true, LV_TEXT_ALIGN_CENTER);
    }
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    return b;
}

// ---- toggle -------------------------------------------------------------------
namespace {
struct ToggleState {
    bool on;
    const Accent *a;
    float k;
    lv_obj_t *knob;
};
void anim_x(void *o, int32_t v)
{
    lv_obj_set_x(static_cast<lv_obj_t *>(o), v);
}
void anim_w(void *o, int32_t v)
{
    lv_obj_set_width(static_cast<lv_obj_t *>(o), v);
}

void toggle_style(lv_obj_t *t, ToggleState *s)
{
    if (s->on) {
        style_fill(t, *s->a, false);
    } else {
        lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_bg_color(t, hex(OFF), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_shadow_opa(t, LV_OPA_TRANSP, 0);
    }
}

void toggle_clicked(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_current_target_obj(e);
    toggle_set(t, !state_of<ToggleState>(t)->on, true);
    lv_obj_send_event(t, LV_EVENT_VALUE_CHANGED, nullptr);
}
}  // namespace

lv_obj_t *toggle(lv_obj_t *parent, int x, int y, bool on, const Accent &a, float k)
{
    lv_obj_t *t = box(parent, x, y, int(56 * k), int(32 * k), LV_RADIUS_CIRCLE);
    auto *s = new ToggleState{on, &a, k, nullptr};
    lv_obj_set_user_data(t, s);
    lv_obj_add_event_cb(t, delete_state<ToggleState>, LV_EVENT_DELETE, s);
    s->knob = box(t, int((on ? 27 : 3) * k), int(3 * k), int(26 * k), int(26 * k), LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(s->knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s->knob, LV_OPA_COVER, 0);
    toggle_style(t, s);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(t, int32_t(8 * k));
    lv_obj_add_event_cb(t, toggle_clicked, LV_EVENT_CLICKED, nullptr);
    return t;
}

bool toggle_on(lv_obj_t *t)
{
    return state_of<ToggleState>(t)->on;
}

void toggle_set(lv_obj_t *t, bool on, bool anim)
{
    ToggleState *s = state_of<ToggleState>(t);
    s->on = on;
    toggle_style(t, s);
    const int32_t to = int32_t((on ? 27 : 3) * s->k);
    lv_anim_delete(s->knob, anim_x);
    if (!anim) {
        lv_obj_set_x(s->knob, to);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s->knob);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_values(&a, lv_obj_get_x(s->knob), to);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

// ---- bar -------------------------------------------------------------------------
namespace {
bool entry_animations = true;
}
void set_entry_animations(bool on)
{
    entry_animations = on;
}

lv_obj_t *bar(lv_obj_t *parent, int x, int y, int w, int h, int pct, const Accent &a, bool grow)
{
    lv_obj_t *track = box(parent, x, y, w, h, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(track, hex(EDGE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    const int fw = std::max(0, std::min(w, w * pct / 100));
    lv_obj_t *fill = box(track, 0, 0, fw, h, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fill, hex(a.glow), 0);
    if (fw < 1) {
        lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
    }
    if (grow && entry_animations && fw > 0) {
        lv_obj_set_width(fill, 1);
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, fill);
        lv_anim_set_exec_cb(&an, anim_w);
        lv_anim_set_values(&an, std::min(h, fw), fw);
        lv_anim_set_duration(&an, 380);
        lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
        lv_anim_start(&an);
    }
    return track;
}

void bar_set(lv_obj_t *b, int pct)
{
    lv_obj_t *fill = lv_obj_get_child(b, 0);
    const int32_t w = lv_obj_get_width(b), fw = std::max<int32_t>(0, std::min<int32_t>(w, w * pct / 100));
    const bool hidden = lv_obj_has_flag(fill, LV_OBJ_FLAG_HIDDEN);
    if (!lv_anim_get(fill, anim_w) && (fw < 1 ? hidden : !hidden && lv_obj_get_style_width(fill, LV_PART_MAIN) == fw)) {
        return;  // as it is: no redraw
    }
    lv_anim_delete(fill, anim_w);
    if (fw < 1) {
        lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(fill, fw);
}

void set_text(lv_obj_t *label, const char *text)
{
    const char *now = lv_label_get_text(label);
    if (!now || strcmp(now, text) != 0) {
        lv_label_set_text(label, text);
    }
}

void set_hidden(lv_obj_t *o, bool hidden)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_bg_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c)) {
        lv_obj_set_style_bg_color(o, c, 0);
    }
}

void set_text_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(o, c, 0);
    }
}

void set_line_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_line_color(o, LV_PART_MAIN), c)) {
        lv_obj_set_style_line_color(o, c, 0);
    }
}

void set_shadow_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_shadow_color(o, LV_PART_MAIN), c)) {
        lv_obj_set_style_shadow_color(o, c, 0);
    }
}

void set_shadow_opa(lv_obj_t *o, lv_opa_t opa)
{
    if (lv_obj_get_style_shadow_opa(o, LV_PART_MAIN) != opa) {
        lv_obj_set_style_shadow_opa(o, opa, 0);
    }
}

// ---- capsule -------------------------------------------------------------------
namespace {
struct CapsuleState {
    int sel, count, pad, seg;
    const Accent *a;
    lv_obj_t *ind;
    lv_obj_t *labels[8];
};

void capsule_clicked(lv_event_t *e)
{
    lv_obj_t *c = lv_event_get_current_target_obj(e);
    CapsuleState *s = state_of<CapsuleState>(c);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(c, &a);
    const int index = std::clamp(int((p.x - a.x1 - s->pad) / std::max(1, s->seg)), 0, s->count - 1);
    if (index == s->sel) {
        return;
    }
    capsule_select(c, index, true);
    lv_obj_send_event(c, LV_EVENT_VALUE_CHANGED, nullptr);
}
}  // namespace

lv_obj_t *capsule(lv_obj_t *parent, int x, int y, int w, int h, const char *const *labels, int count, int selected,
                  const Accent &a, int px)
{
    count = std::clamp(count, 1, 8);
    lv_obj_t *c = box(parent, x, y, w, h, LV_RADIUS_CIRCLE);
    style_glass(c, a);
    auto *s = new CapsuleState{};
    s->count = count;
    s->sel = selected < 0 ? -1 : std::min(selected, count - 1);  // -1: nothing chosen yet
    s->pad = std::max(4, h / 10);
    s->seg = (w - 2 * s->pad) / count;
    s->a = &a;
    lv_obj_set_user_data(c, s);
    lv_obj_add_event_cb(c, delete_state<CapsuleState>, LV_EVENT_DELETE, s);
    s->ind = box(c, s->pad + std::max(0, s->sel) * s->seg, s->pad, s->seg, h - 2 * s->pad, LV_RADIUS_CIRCLE);
    style_fill(s->ind, a, true);
    if (s->sel < 0) {
        lv_obj_add_flag(s->ind, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_shadow_width(s->ind, 16, 0);
    const lv_font_t *f = font(px, true);
    for (int i = 0; i < count; ++i) {
        s->labels[i] = label(c, labels[i], s->pad + i * s->seg, (h - lv_font_get_line_height(f)) / 2, s->seg,
                             i == s->sel ? a.ink : s->sel < 0 ? TEXT : TEXT2, px, true, LV_TEXT_ALIGN_CENTER);
    }
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, capsule_clicked, LV_EVENT_CLICKED, nullptr);
    return c;
}

int capsule_selected(lv_obj_t *c)
{
    return state_of<CapsuleState>(c)->sel;
}

void capsule_select(lv_obj_t *c, int index, bool anim)
{
    CapsuleState *s = state_of<CapsuleState>(c);
    index = std::clamp(index, 0, s->count - 1);
    for (int i = 0; i < s->count; ++i) {
        lv_obj_set_style_text_color(s->labels[i], hex(i == index ? s->a->ink : TEXT2), 0);
    }
    const bool was_hidden = lv_obj_has_flag(s->ind, LV_OBJ_FLAG_HIDDEN);
    s->sel = index;
    lv_obj_remove_flag(s->ind, LV_OBJ_FLAG_HIDDEN);
    const int32_t to = s->pad + index * s->seg;
    lv_anim_delete(s->ind, anim_x);
    if (!anim || was_hidden) {
        lv_obj_set_x(s->ind, to);
        return;
    }
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, s->ind);
    lv_anim_set_exec_cb(&an, anim_x);
    lv_anim_set_values(&an, lv_obj_get_x(s->ind), to);
    lv_anim_set_duration(&an, 260);
    lv_anim_set_path_cb(&an, lv_anim_path_overshoot);
    lv_anim_start(&an);
}

// ---- dot and icons ---------------------------------------------------------------
lv_obj_t *dot(lv_obj_t *parent, int x, int y, int size, uint32_t color, bool glow)
{
    lv_obj_t *d = box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(d, hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    (void)glow;  // flat: no halo
    return d;
}

namespace {
constexpr float house_0[] = {3, 11, 12, 4, 21, 11};
constexpr float house_1[] = {5, 10, 5, 20, 10, 20, 10, 14, 14, 14, 14, 20, 19, 20, 19, 10};
constexpr home_icons::Path house_paths[] = {{house_0, 3}, {house_1, 8}};
constexpr float sl_0[] = {4, 6, 20, 6}, sl_1[] = {4, 12, 20, 12}, sl_2[] = {4, 18, 20, 18};
constexpr float sl_3[] = {9, 3.5f, 9, 8.5f}, sl_4[] = {15, 9.5f, 15, 14.5f}, sl_5[] = {7, 15.5f, 7, 20.5f};
constexpr home_icons::Path slider_paths[] = {{sl_0, 2}, {sl_1, 2}, {sl_2, 2}, {sl_3, 2}, {sl_4, 2}, {sl_5, 2}};
constexpr float folder_0[] = {3, 6, 9, 6, 11, 9, 21, 9, 21, 19, 3, 19, 3, 6};
constexpr home_icons::Path folder_paths[] = {{folder_0, 7}};
constexpr float file_0[] = {6, 3, 14, 3, 19, 8, 19, 21, 6, 21, 6, 3};
constexpr float file_1[] = {14, 3, 14, 8, 19, 8};
constexpr home_icons::Path file_paths[] = {{file_0, 6}, {file_1, 3}};
constexpr float image_0[] = {3, 5, 21, 5, 21, 19, 3, 19, 3, 5};
constexpr float image_1[] = {3, 17, 9, 11, 13, 15, 16, 12, 21, 17};
constexpr float image_2[] = {16, 8.5f, 16.5f, 8.5f};
constexpr home_icons::Path image_paths[] = {{image_0, 5}, {image_1, 5}, {image_2, 2}};
constexpr float compass_0[] = {21, 12, 20.56f, 14.78f, 19.28f, 17.29f, 17.29f, 19.28f, 14.78f, 20.56f, 12, 21, 9.22f, 20.56f, 6.71f, 19.28f, 4.72f, 17.29f, 3.44f, 14.78f, 3, 12, 3.44f, 9.22f, 4.72f, 6.71f, 6.71f, 4.72f, 9.22f, 3.44f, 12, 3, 14.78f, 3.44f, 17.29f, 4.72f, 19.28f, 6.71f, 20.56f, 9.22f, 21, 12};
constexpr float compass_1[] = {12, 6, 14, 12, 12, 18, 10, 12, 12, 6};
constexpr home_icons::Path compass_paths[] = {{compass_0, 21}, {compass_1, 5}};
constexpr float level_0[] = {3, 9, 21, 9, 21, 15, 3, 15, 3, 9};
constexpr float level_1[] = {13.6f, 12, 13.29f, 12.94f, 12.49f, 13.52f, 11.51f, 13.52f, 10.71f, 12.94f, 10.4f, 12, 10.71f, 11.06f, 11.51f, 10.48f, 12.49f, 10.48f, 13.29f, 11.06f, 13.6f, 12};
constexpr float level_2[] = {9, 9, 9, 15};
constexpr float level_3[] = {15, 9, 15, 15};
constexpr home_icons::Path level_paths[] = {{level_0, 5}, {level_1, 11}, {level_2, 2}, {level_3, 2}};
constexpr float calc_0[] = {5, 3, 19, 3, 19, 21, 5, 21, 5, 3};
constexpr float calc_1[] = {8, 6, 16, 6, 16, 9, 8, 9, 8, 6};
constexpr float calc_2[] = {8, 13, 9.5f, 13};
constexpr float calc_3[] = {11.25f, 13, 12.75f, 13};
constexpr float calc_4[] = {14.5f, 13, 16, 13};
constexpr float calc_5[] = {8, 17, 9.5f, 17};
constexpr float calc_6[] = {11.25f, 17, 12.75f, 17};
constexpr float calc_7[] = {14.5f, 17, 16, 17};
constexpr home_icons::Path calc_paths[] = {{calc_0, 5}, {calc_1, 5}, {calc_2, 2}, {calc_3, 2}, {calc_4, 2}, {calc_5, 2}, {calc_6, 2}, {calc_7, 2}};
constexpr float torch_0[] = {9, 3, 15, 3, 15, 7, 13, 11, 13, 21, 11, 21, 11, 11, 9, 7, 9, 3};
constexpr float torch_1[] = {9, 7, 15, 7};
constexpr float torch_2[] = {12, 14, 12, 16};
constexpr home_icons::Path torch_paths[] = {{torch_0, 9}, {torch_1, 2}, {torch_2, 2}};
constexpr float share_0[] = {4, 8, 4, 20, 20, 20, 20, 8};
constexpr float share_1[] = {12, 3, 12, 14};
constexpr float share_2[] = {8, 7, 12, 3, 16, 7};
constexpr home_icons::Path share_paths[] = {{share_0, 4}, {share_1, 2}, {share_2, 3}};
constexpr float trash_0[] = {4, 7, 20, 7}, trash_1[] = {9, 7, 9, 4, 15, 4, 15, 7}, trash_2[] = {6, 7, 7, 20, 17, 20, 18, 7};
constexpr float trash_3[] = {10, 11, 10, 16}, trash_4[] = {14, 11, 14, 16};
constexpr home_icons::Path trash_paths[] = {{trash_0, 2}, {trash_1, 4}, {trash_2, 4}, {trash_3, 2}, {trash_4, 2}};
constexpr float photos_0[] = {3, 6, 17, 6, 17, 18, 3, 18, 3, 6}, photos_1[] = {7, 3, 21, 3, 21, 15}, photos_2[] = {3, 16, 8, 11, 11, 14, 13, 12, 17, 16};
constexpr home_icons::Path photos_paths[] = {{photos_0, 5}, {photos_1, 3}, {photos_2, 5}};
constexpr home_icons::Icon extra[] = {{"house", house_paths, 2}, {"sliders", slider_paths, 6}, {"folder", folder_paths, 1}, {"file", file_paths, 2}, {"image", image_paths, 3}, {"compass", compass_paths, 2}, {"level", level_paths, 4}, {"calc", calc_paths, 8}, {"torch", torch_paths, 3}, {"share", share_paths, 3}, {"trash", trash_paths, 5}, {"photos", photos_paths, 3}};

void draw_icon(lv_event_t *e)
{
    auto *ic = static_cast<const home_icons::Icon *>(lv_event_get_user_data(e));
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    draw_icon_mask(lv_event_get_layer(e), a, ic, lv_obj_get_style_line_color(o, LV_PART_MAIN),
                   lv_obj_get_style_line_width(o, LV_PART_MAIN), lv_obj_get_style_line_opa(o, LV_PART_MAIN));
}
}  // namespace

// Rasterised icons: per (icon, size, stroke), an A8 mask of round-capped
// strokes with a one-pixel anti-aliased edge.
const lv_image_dsc_t *icon_mask(const home_icons::Icon *ic, int size, int width)
{
    struct Key {
        const home_icons::Icon *icon;
        int size, width;
        bool operator<(const Key &o) const
        {
            return std::tie(icon, size, width) < std::tie(o.icon, o.size, o.width);
        }
    };
    static std::map<Key, lv_image_dsc_t *> cache;
    const Key key{ic, size, width};
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
#ifdef ESP_PLATFORM
    auto *px = static_cast<uint8_t *>(heap_caps_calloc(1, size_t(size) * size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    auto *px = static_cast<uint8_t *>(calloc(1, size_t(size) * size));
#endif
    if (!px) {
        return nullptr;
    }
    auto *dsc = new lv_image_dsc_t{};
    const float k = float(size) / 24.0f, r = width / 2.0f;
    struct Seg {
        float x1, y1, dx, dy, len2;
    };
    std::vector<Seg> segs;
    for (uint8_t p = 0; p < ic->count; ++p) {
        const auto &path = ic->paths[p];
        for (uint16_t i = 1; i < path.points; ++i) {
            const float x1 = path.xy[2 * i - 2] * k, y1 = path.xy[2 * i - 1] * k;
            const float dx = path.xy[2 * i] * k - x1, dy = path.xy[2 * i + 1] * k - y1;
            segs.push_back({x1, y1, dx, dy, dx * dx + dy * dy});
        }
    }
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float cx = x + 0.5f, cy = y + 0.5f;
            float best = 1e9f;
            for (const Seg &s : segs) {
                float t = s.len2 > 0 ? ((cx - s.x1) * s.dx + (cy - s.y1) * s.dy) / s.len2 : 0;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                const float ex = s.x1 + s.dx * t - cx, ey = s.y1 + s.dy * t - cy;
                best = std::min(best, ex * ex + ey * ey);
            }
            const float cover = r + 0.5f - sqrtf(best);
            px[y * size + x] = uint8_t(cover <= 0 ? 0 : cover >= 1 ? 255 : int(cover * 255));
        }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_A8;
    dsc->header.w = uint32_t(size);
    dsc->header.h = uint32_t(size);
    dsc->header.stride = uint32_t(size);
    dsc->data = px;
    dsc->data_size = uint32_t(size * size);
    cache[key] = dsc;
    return dsc;
}

void draw_icon_mask(lv_layer_t *layer, const lv_area_t &a, const home_icons::Icon *ic, lv_color_t color, int width, lv_opa_t opa)
{
    const int size = std::max(1, int(lv_area_get_width(&a)));
    const lv_image_dsc_t *mask = icon_mask(ic, size, std::max(1, width));
    if (!mask) {
        return;
    }
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = mask;
    d.recolor = color;
    d.recolor_opa = LV_OPA_COVER;
    d.opa = opa;
    lv_area_t area = {a.x1, a.y1, a.x1 + size - 1, a.y1 + size - 1};
    lv_draw_image(layer, &d, &area);
}

lv_obj_t *icon(lv_obj_t *parent, const char *name, int x, int y, int size, uint32_t color, float stroke)
{
    lv_obj_t *o = box(parent, x, y, size, size);
    const home_icons::Icon *found = home_icons::find(name);
    for (const auto &ic : extra)
        if (!strcmp(ic.name, name)) {
            found = &ic;
        }
    if (!found) {
        return o;
    }
    lv_obj_set_style_line_color(o, hex(color), 0);
    lv_obj_set_style_line_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_line_width(o, std::max(1, int(lroundf(stroke * size / 24.0f))), 0);
    lv_obj_add_event_cb(o, draw_icon, LV_EVENT_DRAW_MAIN, const_cast<home_icons::Icon *>(found));
    return o;
}

// ---- sparkline ----------------------------------------------------------------------
namespace {
struct SparkState {
    float v[64];
    int n = 0;
    float max = 100;
    uint32_t color;
    int32_t reveal = 1024;  // 0..1024
};

void draw_spark(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    SparkState *s = state_of<SparkState>(o);
    if (s->n < 2) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    const float step = float(w - 1) / float(s->n - 1);
    auto yat = [&](float x) {
        const float f = std::clamp(x / step, 0.0f, float(s->n - 1));
        const int i = std::min(int(f), s->n - 2);
        const float v = s->v[i] + (s->v[i + 1] - s->v[i]) * (f - i);
        return float(a.y1) + 4 + (1 - std::clamp(v / s->max, 0.0f, 1.0f)) * float(h - 8);
    };
    const int32_t limit = w * s->reveal / 1024;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = hex(s->color);
    r.bg_opa = 38;
    for (int32_t x = 0; x < limit; x += 3) {
        lv_area_t col = {a.x1 + x, int32_t(lroundf(yat(float(x) + 1))), std::min(a.x1 + x + 2, a.x2), a.y2};
        if (col.y1 < col.y2) {
            lv_draw_rect(layer, &r, &col);
        }
    }
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = hex(s->color);
    line.width = 3;
    line.round_start = line.round_end = true;
    for (lv_draw_line_dsc_t *d : {
                &line
            }) {
        for (int i = 1; i < s->n; ++i) {
            const float x0 = (i - 1) * step, x1 = std::min(i * step, float(limit));
            if (x0 >= limit) {
                break;
            }
            d->p1 = {lv_value_precise_t(lroundf(a.x1 + x0)), lv_value_precise_t(lroundf(yat(x0)))};
            d->p2 = {lv_value_precise_t(lroundf(a.x1 + x1)), lv_value_precise_t(lroundf(yat(x1)))};
            lv_draw_line(layer, d);
        }
    }
}

void spark_reveal(void *o, int32_t v)
{
    state_of<SparkState>(static_cast<lv_obj_t *>(o))->reveal = v;
    lv_obj_invalidate(static_cast<lv_obj_t *>(o));
}
}  // namespace

lv_obj_t *spark(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = box(parent, x, y, w, h);
    auto *s = new SparkState{};
    s->color = color;
    s->reveal = 0;
    lv_obj_set_user_data(o, s);
    lv_obj_add_event_cb(o, delete_state<SparkState>, LV_EVENT_DELETE, s);
    lv_obj_add_event_cb(o, draw_spark, LV_EVENT_DRAW_MAIN, nullptr);
    if (!entry_animations) {
        s->reveal = 1024;
        return o;
    }
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, o);
    lv_anim_set_exec_cb(&an, spark_reveal);
    lv_anim_set_values(&an, 0, 1024);
    lv_anim_set_duration(&an, 520);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
    lv_anim_start(&an);
    return o;
}

void spark_set(lv_obj_t *o, const float *values, int count, float max_value)
{
    SparkState *s = state_of<SparkState>(o);
    count = std::min(count, 64);
    if (count == s->n && max_value == s->max && !memcmp(values, s->v, sizeof(float) * size_t(count))) {
        return;
    }
    memcpy(s->v, values, sizeof(float) * size_t(count));
    s->n = count;
    s->max = max_value > 0 ? max_value : 1;
    lv_obj_invalidate(o);
}

// ---- card fill -----------------------------------------------------------------------
namespace {
struct FillState {
    int pct;
    uint32_t top, bottom;
};

void draw_fill(lv_event_t *e)
{
    auto *s = static_cast<FillState *>(lv_event_get_user_data(e));
    if (s->pct <= 0) {
        return;
    }
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int32_t w = lv_area_get_width(&a);
    lv_area_t fill = a;
    fill.x2 = a.x1 + std::max<int32_t>(1, w * std::min(s->pct, 100) / 100) - 1;
    const lv_area_t saved = layer->_clip_area;
    const lv_area_t clip = {std::max(saved.x1, fill.x1), std::max(saved.y1, fill.y1), std::min(saved.x2, fill.x2),
                            std::min(saved.y2, fill.y2)
                           };
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) {
        return;
    }
    layer->_clip_area = clip;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = lv_obj_get_style_radius(o, LV_PART_MAIN);
    d.bg_color = hex(s->bottom);
    lv_draw_rect(layer, &d, &a);
    layer->_clip_area = saved;
}
}  // namespace

void card_fill(lv_obj_t *card, int pct, uint32_t top, uint32_t bottom)
{
    auto *s = new FillState{pct, top, bottom};
    lv_obj_add_event_cb(card, draw_fill, LV_EVENT_DRAW_MAIN, s);
    lv_obj_add_event_cb(card, delete_state<FillState>, LV_EVENT_DELETE, s);
}

void card_fill_set(lv_obj_t *card, int pct, uint32_t top, uint32_t bottom)
{
    const uint32_t n = lv_obj_get_event_count(card);
    for (uint32_t i = 0; i < n; ++i) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(card, i);
        if (lv_event_dsc_get_cb(d) == draw_fill) {
            auto *s = static_cast<FillState *>(lv_event_dsc_get_user_data(d));
            if (s->pct != pct || s->top != top || s->bottom != bottom) {
                *s = {pct, top, bottom};
                lv_obj_invalidate(card);
            }
            return;
        }
    }
}

// ---- motion -----------------------------------------------------------------------------
namespace {
void rise(void *o, int32_t v)
{
    auto *obj = static_cast<lv_obj_t *>(o);
    lv_obj_set_style_translate_y(obj, 8 * (256 - v) / 256, 0);
    lv_obj_set_style_opa(obj, lv_opa_t(std::clamp<int32_t>(v, 0, 255)), 0);
}
}  // namespace

void stagger(lv_obj_t *parent, uint32_t delay_ms, uint32_t step_ms)
{
    const uint32_t n = lv_obj_get_child_count(parent);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, int32_t(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_anim_delete(child, rise);
        rise(child, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, child);
        lv_anim_set_exec_cb(&a, rise);
        lv_anim_set_values(&a, 0, 256);
        lv_anim_set_duration(&a, 180);
        // Quick cascade: later children start together so a page is whole fast.
        lv_anim_set_delay(&a, delay_ms + step_ms * std::min<uint32_t>(k++, 6));
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

}  // namespace mui
