// SPDX-License-Identifier: Apache-2.0
// Mosaico AMOLED UI kit, after Vibe Mode's own design: true-black panel, flat
// near-black cards with a hairline edge, one solid accent per app, bold Inter,
// and spring motion (sliding capsule pills, toggles, squash on press, rise-in).
// All calls run on the LVGL task (or with its lock held).
#pragma once

#include <stdint.h>

#include "lvgl.h"

namespace home_icons {
struct Icon;
}

namespace mui {

constexpr int PANEL = 480;
constexpr uint32_t TEXT = 0xF4F4F5, TEXT2 = 0xA6A6AD, TEXT3 = 0x75757C, OK = 0x7AD6A0, WARN = 0xF0B858,
                   RED = 0xFF8078, OFF = 0x404040;
// Vibe Mode surfaces: card, its hairline edge, a raised control.
constexpr uint32_t CARD = 0x181818, EDGE = 0x303030, FIELD = 0x282828;  // neutral in RGB565

// One accent per app: gradient pair (hi -> lo), glow, ink on the gradient, and
// the glass tint pair.
struct Accent {
    uint32_t hi, lo, glow, ink, glass_a, glass_b;
};
extern const Accent VIOLET, CYAN, AMBER, GREEN;

// Inter (SemiBold or Regular) at a pixel size, with the built-in Montserrat
// symbols as fallback. Cached for the life of the firmware.
const lv_font_t *font(int px, bool semi = true);
int text_width(const char *text, int px, bool semi = true);

// Panel-sized background image: black, the accent's horizon glow, optional
// diagonal rays and a bloom of hue bloom_hue (0-359, -1 none) centred at
// (bloom_x, bloom_y). Rendered once per combination into PSRAM.
struct Backdrop {
    const Accent *accent;
    bool rays = true;
    int horizon_y = 505;  // centre of the horizon ellipse
    int bloom_hue = -1, bloom_x = 70, bloom_y = 90;
};
lv_obj_t *backdrop(lv_obj_t *parent, const Backdrop &b);

// Plain transparent container (not clickable, not scrollable).
lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, int radius = 0);
lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w, uint32_t color, int px, bool semi = false,
                lv_text_align_t align = LV_TEXT_ALIGN_LEFT);
// A label() that wraps onto as many lines as its text needs (label() is one line high).
void wrap_label(lv_obj_t *l);

// Glass card. style_glass restyles an existing object the same way.
lv_obj_t *glass(lv_obj_t *parent, int x, int y, int w, int h, int radius, const Accent &a);
void style_glass(lv_obj_t *o, const Accent &a, lv_opa_t strength = LV_OPA_COVER);
// Undo style_glass/style_fill (solid background, no edge, glow or squash).
void clear_glass(lv_obj_t *o);
// Gradient pill (button fill, selected tab) with an optional glow.
void style_fill(lv_obj_t *o, const Accent &a, bool glow = true);
// Bright 1 px top edge fading to the right, drawn over the object.
void lit_edge(lv_obj_t *o);

lv_obj_t *fill_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, const Accent &a, int px = 16);
lv_obj_t *field_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int px = 16,
                       uint32_t color = TEXT);

// Squash on press (springs back on release).
void press_fx(lv_obj_t *o);

// Toggle 56x32 (scaled by k): sends LV_EVENT_VALUE_CHANGED after a tap.
lv_obj_t *toggle(lv_obj_t *parent, int x, int y, bool on, const Accent &a, float k = 1.0f);
bool toggle_on(lv_obj_t *t);
void toggle_set(lv_obj_t *t, bool on, bool anim);

// Rounded progress bar: gradient fill with glow; grows in when created.
// Bars grow and sparklines draw themselves in when created. A page that is
// rebuilt with fresh data (rather than opened) turns that off meanwhile, so
// refreshes do not replay the entry animations.
void set_entry_animations(bool on);
lv_obj_t *bar(lv_obj_t *parent, int x, int y, int w, int h, int pct, const Accent &a, bool grow = true);
void bar_set(lv_obj_t *b, int pct);

// Segmented capsule with a springing indicator (selected -1: none yet).
// Sends VALUE_CHANGED.
lv_obj_t *capsule(lv_obj_t *parent, int x, int y, int w, int h, const char *const *labels, int count, int selected,
                  const Accent &a, int px = 16);
int capsule_selected(lv_obj_t *c);
void capsule_select(lv_obj_t *c, int index, bool anim);

// Status dot; glow adds a soft halo in its own colour.
lv_obj_t *dot(lv_obj_t *parent, int x, int y, int size, uint32_t color, bool glow = true);

// Stroke icon on a 24-unit grid: Swifty Home's set (home_icons.h) plus
// "house" and "sliders".
lv_obj_t *icon(lv_obj_t *parent, const char *name, int x, int y, int size, uint32_t color, float stroke = 1.9f);
// Draws a line icon into area. It is rasterised once per size and stroke into
// an alpha mask and then blended as one image: drawing its polylines segment
// by segment cost a draw task per segment per render strip, and made
// scrolling slow.
void draw_icon_mask(lv_layer_t *layer, const lv_area_t &area, const home_icons::Icon *icon, lv_color_t color, int width,
                    lv_opa_t opa = LV_OPA_COVER);

// Area sparkline: glowing line over a fading fill; draws in left to right.
lv_obj_t *spark(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color);
void spark_set(lv_obj_t *s, const float *values, int count, float max_value);

// Left-to-right fill inside a rounded card (clipped to its corners) in the
// solid colour `bottom` (`top` is kept for callers that pass a pair).
void card_fill(lv_obj_t *card, int pct, uint32_t top, uint32_t bottom);
void card_fill_set(lv_obj_t *card, int pct, uint32_t top, uint32_t bottom);

// Children rise into place one after another.
void stagger(lv_obj_t *parent, uint32_t delay_ms = 0, uint32_t step_ms = 18);

// ---- keyboard (keyboard.cpp) ----------------------------------------------------------
// An iOS-style keyboard that types into textarea and sends it LV_EVENT_READY
// from the action key. About 480 x 236 fills the bottom of the panel.
struct KeyboardConfig {
    const char *action = "Done";   // the action key's label
    const Accent *accent = &CYAN;  // its colour
    bool caps_only = false;        // capitals only (pairing codes)
    bool auto_caps = true;         // a capital to start a sentence; off for URLs and tokens
    int side_inset = 6;            // space at the left and right
    int bottom_inset = 46;         // extra inset of the bottom row, clear of the rounded corners
};
lv_obj_t *keyboard(lv_obj_t *parent, lv_obj_t *textarea, int x, int y, int w, int h, const KeyboardConfig &config);
// A tick for each key press (the shell's haptics).
void keyboard_set_haptic(void (*tick)());

}  // namespace mui
