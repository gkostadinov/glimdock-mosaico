// SPDX-License-Identifier: Apache-2.0
#include "launcher_ui.h"
#include "tools_app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "mui.h"

namespace launcher_brand {
#include "launcher_brand.h"
}

namespace {
using namespace mui;
constexpr int MIN_RADIUS = 0, MAX_RADIUS = 160;
// Swifty's sky blue, from the bird's own colours; Files' teal; Tools' coral.
const Accent SKY = {0x7DF3FF, 0x2B5CF2, 0x35B2FF, 0x041428, 0x12305A, 0x060E1C};
const Accent TEAL = {0x8CF5DC, 0x139E86, 0x3DDCBB, 0x022019, 0x0C2E28, 0x061613};
const Accent CORAL = {0xFFB199, 0xE0533D, 0xFF8A70, 0x2A0A04, 0x3A1610, 0x1A0906};
const Accent GRAPHITE = {0xC9C9CF, 0x5A5A62, 0xB0B0B8, 0x101012, 0x2A2A2E, 0x151517};

// The home screen: status bar, two live widgets, and app icons in a grid,
// like a phone's. Settings opens as a sheet over it.
enum Slot { S_GLIMDOCK, S_HOME, S_SWIFTY, S_CAMERA, S_GALLERY, S_FILES, S_TOOLS, S_SETTINGS, SLOTS };
struct AppIcon {
    const char *name, *glyph;
    const Accent *accent;
    int app;  // ShellApp, or -1 Settings, -2 the calculator
};
const AppIcon APPS[SLOTS] = {
    {"Glimdock", nullptr, &VIOLET, int(ShellApp::Glimdock)}, {"Home", "house", &VIOLET, int(ShellApp::Home)},
    {"Swifty", "mic", &SKY, int(ShellApp::Swifty)},          {"Camera", "camera", &AMBER, int(ShellApp::Camera)},
    {"Gallery", "photos", &AMBER, int(ShellApp::Gallery)},   {"Files", "folder", &TEAL, int(ShellApp::Files)},
    {"Tools", "torch", &CORAL, int(ShellApp::Tools)},        {"Settings", "sliders", &GREEN, -1},
};
constexpr int ICON = 72, PITCH = 100, GRID_X = 40, GRID_Y = 194, ROW = 104;

struct {
    LauncherHooks hooks{};
    int radius = 80, draft = 80;
    lv_obj_t *screen = nullptr, *home = nullptr, *grid = nullptr, *widgets = nullptr, *settings = nullptr;
    lv_obj_t *clock = nullptr, *wifi = nullptr, *date = nullptr, *badge = nullptr, *badge_text = nullptr, *swifty_dot = nullptr;
    lv_obj_t *batt = nullptr, *batt_fill = nullptr, *batt_nub = nullptr, *batt_bolt = nullptr, *picker = nullptr;
    // The two widgets: their kind and parts.
    struct {
        int kind;
        lv_obj_t *card, *title, *big, *line1, *line2, *icon, *spark, *dot;
    } w[2] = {};
    // The latest data, so a widget changed by the person fills at once.
    glimdock_summary_t glim{};
    home_summary_t home_data{};
    swifty_summary_t swifty{};
    char clock_text[8] = "--:--", date_text[24] = "";
    int battery = -1;
    bool charging = false, have_data = false;
    // Settings sheet
    lv_obj_t *s_bright = nullptr, *s_bright_bar = nullptr, *s_radius = nullptr, *s_wifi_dot = nullptr,
             *s_ssid = nullptr, *s_detail = nullptr, *s_wifi_state = nullptr, *s_versions = nullptr, *s_zone = nullptr,
             *s_cam_auto = nullptr;
    // Corner calibration
    lv_obj_t *calibration = nullptr, *guide = nullptr, *cal_value = nullptr;
} ui;

void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

void set_color(lv_obj_t *o, uint32_t color)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), lv_color_hex(color)))
        lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
}

void on(lv_obj_t *o, lv_event_cb_t cb, lv_event_code_t code = LV_EVENT_CLICKED, void *data = nullptr)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, cb, code, data);
}

void open_app(int app)
{
    if (app == -1) {
        lv_obj_scroll_to_y(ui.settings, 0, LV_ANIM_OFF);
        lv_obj_remove_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui.settings);
        stagger(ui.settings, 0, 15);
        return;
    }
    if (app == -2) {
        tools_app_select(3);
        app = int(ShellApp::Tools);
    }
    if (ui.hooks.open) ui.hooks.open(ShellApp(app));
}

void open_cb(lv_event_t *e) { open_app(int(intptr_t(lv_event_get_user_data(e)))); }

void draw_g(lv_event_t *e)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
    const auto &g = launcher_brand::glimdock::MARK_G;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &g;
    d.recolor = lv_color_white();
    d.recolor_opa = LV_OPA_COVER;
    const int32_t x = a.x1 + (lv_area_get_width(&a) - int32_t(g.header.w)) / 2;
    const int32_t y = a.y1 + (lv_area_get_height(&a) - int32_t(g.header.h)) / 2 - 2;
    lv_area_t area = {x, y, x + int32_t(g.header.w) - 1, y + int32_t(g.header.h) - 1};
    lv_draw_image(lv_event_get_layer(e), &d, &area);
}

uint32_t hsl(float h, float s, float l)
{
    h = fmodf(fmodf(h, 360) + 360, 360) / 360;
    auto f = [&](float n) {
        const float k = fmodf(n + h * 12, 12), a = s * std::min(l, 1 - l);
        return l - a * std::max(-1.0f, std::min({k - 3, 9 - k, 1.0f}));
    };
    return uint32_t(lroundf(f(0) * 255)) << 16 | uint32_t(lroundf(f(8) * 255)) << 8 | uint32_t(lroundf(f(4) * 255));
}

// ---- icons ----------------------------------------------------------------------------------
void build_grid()
{
    // Each cell has room above its icon for the badge: LVGL redraws a child
    // only inside its parents, so a badge poking out of the cell was cut off
    // whenever it changed.
    constexpr int TOP = 12;
    ui.grid = box(ui.home, 0, GRID_Y - TOP, PANEL, 2 * ROW + TOP);
    for (int i = 0; i < SLOTS; ++i) {
        const AppIcon &a = APPS[i];
        const int cx = GRID_X + (i % 4) * PITCH + PITCH / 2, y = (i / 4) * ROW;
        lv_obj_t *cell = box(ui.grid, cx - PITCH / 2, y, PITCH, ROW + TOP);
        lv_obj_t *ic = box(cell, (PITCH - ICON) / 2, TOP, ICON, ICON, 20);
        style_fill(ic, *a.accent, false);
        if (a.glyph) icon(ic, a.glyph, 16, 16, 40, 0xFFFFFF, 2.0f);
        else lv_obj_add_event_cb(ic, draw_g, LV_EVENT_DRAW_MAIN, nullptr);
        label(cell, a.name, 0, TOP + ICON + 6, PITCH, TEXT, 13, true, LV_TEXT_ALIGN_CENTER);
        press_fx(ic);
        on(ic, open_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(a.app)));
        if (i == S_GLIMDOCK) {  // a badge with the alert count
            ui.badge = box(cell, (PITCH + ICON) / 2 - 16, TOP - 10, 26, 26, 13);
            lv_obj_set_style_bg_color(ui.badge, lv_color_hex(0xFF3B30), 0);
            lv_obj_set_style_bg_opa(ui.badge, LV_OPA_COVER, 0);
            ui.badge_text = label(ui.badge, "", 0, 4, 26, 0xFFFFFF, 13, true, LV_TEXT_ALIGN_CENTER);
            lv_obj_add_flag(ui.badge, LV_OBJ_FLAG_HIDDEN);
        } else if (i == S_SWIFTY) {  // listening or thinking
            ui.swifty_dot = box(cell, (PITCH + ICON) / 2 - 10, TOP - 4, 14, 14, 7);
            lv_obj_set_style_bg_color(ui.swifty_dot, lv_color_hex(SKY.glow), 0);
            lv_obj_set_style_bg_opa(ui.swifty_dot, LV_OPA_COVER, 0);
            lv_obj_add_flag(ui.swifty_dot, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---- widgets ---------------------------------------------------------------------------------
// Two slots above the icons; hold one to choose what it shows.
enum WidgetKind { W_GLIMDOCK, W_LIGHTS, W_MUSIC, W_SWIFTY, W_BATTERY, W_CLOCK, W_KINDS };
const char *const WIDGET_NAMES[W_KINDS] = {"Glimdock", "Lights", "Music", "Swifty", "Battery", "Clock"};
const char *const WIDGET_ICONS[W_KINDS] = {"tech", "bulb", "speaker", "mic", "power", "clock"};
const int WIDGET_APP[W_KINDS] = {int(ShellApp::Glimdock), int(ShellApp::Home), int(ShellApp::Home),
                                 int(ShellApp::Swifty), -3, -3};
constexpr int WX = 40, WY = 44, WW = 196, WH = 136, WGAP = 8;

void update_widget(int slot);
void open_picker(int slot);

void widget_tap_cb(lv_event_t *e)
{
    const int slot = int(intptr_t(lv_event_get_user_data(e)));
    const int app = WIDGET_APP[ui.w[slot].kind];
    if (app >= 0) open_app(app);
}

void widget_hold_cb(lv_event_t *e) { open_picker(int(intptr_t(lv_event_get_user_data(e)))); }

void build_widget(int slot)
{
    auto &w = ui.w[slot];
    if (w.card) lv_obj_delete(w.card);
    const int kind = w.kind;
    const Accent &acc = kind == W_GLIMDOCK ? CYAN : kind == W_SWIFTY ? SKY : kind == W_BATTERY ? GREEN
                      : kind == W_CLOCK ? GRAPHITE : VIOLET;
    w = {kind, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    w.card = glass(ui.home, WX + slot * (WW + WGAP), WY, WW, WH, 26, acc);
    press_fx(w.card);
    lv_obj_add_flag(w.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(w.card, widget_tap_cb, LV_EVENT_SHORT_CLICKED, reinterpret_cast<void *>(intptr_t(slot)));
    lv_obj_add_event_cb(w.card, widget_hold_cb, LV_EVENT_LONG_PRESSED, reinterpret_cast<void *>(intptr_t(slot)));
    if (kind == W_LIGHTS || kind == W_MUSIC || kind == W_BATTERY) card_fill(w.card, 0, 0xFFD9A0, 0xF3B56A);
    if (kind == W_GLIMDOCK) {
        w.dot = dot(w.card, 16, 18, 9, OFF, false);
        w.title = label(w.card, "glimdock", 32, 12, 150, TEXT2, 14, true);
    } else {
        w.icon = icon(w.card, WIDGET_ICONS[kind], 14, 12, 22, TEXT, 1.6f);
        w.title = label(w.card, WIDGET_NAMES[kind], 42, 13, 140, TEXT2, 14, true);
    }
    if (kind == W_SWIFTY) {
        w.line1 = label(w.card, "", 16, 42, WW - 30, TEXT, 15, false);
        lv_label_set_long_mode(w.line1, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(w.line1, 82);
    } else {
        w.big = label(w.card, "\xE2\x80\x94", 16, 34, WW - 26, TEXT, 34, true);
        w.line1 = label(w.card, "", 16, 78, WW - 26, TEXT2, 13, true);
        w.line2 = label(w.card, "", 16, 104, WW - 26, TEXT3, 13, true);
    }
    if (kind == W_GLIMDOCK) {
        lv_obj_add_flag(w.line2, LV_OBJ_FLAG_HIDDEN);
        w.spark = spark(w.card, 1, 96, WW - 2, 38, CYAN.glow);
    }
    if (ui.have_data) update_widget(slot);
}

// Ink that stays readable over a card's fill.
void ink(int slot, bool dark)
{
    auto &w = ui.w[slot];
    const uint32_t c = 0x2A1A05;
    if (w.icon) lv_obj_set_style_line_color(w.icon, lv_color_hex(dark ? c : TEXT), 0);
    set_color(w.title, dark ? c : TEXT2);
    if (w.big) set_color(w.big, dark ? c : TEXT);
    set_color(w.line1, dark ? c : TEXT2);
    if (w.line2) set_color(w.line2, dark ? c : TEXT3);
}

void update_widget(int slot)
{
    auto &w = ui.w[slot];
    char v[64];
    switch (w.kind) {
    case W_GLIMDOCK: {
        const glimdock_summary_t &g = ui.glim;
        if (isfinite(g.cpu)) snprintf(v, sizeof(v), "%d%% CPU", int(lroundf(g.cpu)));
        else strlcpy(v, g.started ? "\xE2\x80\x94" : "Homelab", sizeof(v));
        set_text(w.big, v);
        if (isfinite(g.memory)) snprintf(v, sizeof(v), "Memory %d%%", int(lroundf(g.memory)));
        else strlcpy(v, g.status, sizeof(v));
        set_text(w.line1, v);
        lv_obj_set_style_bg_color(w.dot, lv_color_hex(!g.ready ? OFF : g.alerts ? RED : OK), 0);
        float top = 40;
        for (int i = 0; i < g.history_count; ++i) top = std::max(top, g.history[i] * 1.25f);
        spark_set(w.spark, g.history, g.history_count, std::min(100.0f, top));
        break;
    }
    case W_LIGHTS: {
        const home_summary_t &h = ui.home_data;
        const bool warm = !h.lights || (h.lights_hue >= 20 && h.lights_hue <= 50);
        const int lp = h.lights && h.lights_on ? h.lights_pct : 0;
        card_fill_set(w.card, lp, warm ? 0xFFD9A0 : hsl(float(h.lights_hue), 0.85f, 0.76f),
                      warm ? 0xF3B56A : hsl(float(h.lights_hue), 0.85f, 0.62f));
        ink(slot, lp > 45);
        if (!h.paired) strlcpy(v, h.started ? "Pair" : "Home", sizeof(v));
        else if (h.lights && h.lights_on) snprintf(v, sizeof(v), "%d%%", h.lights_pct);
        else strlcpy(v, h.lights ? "Off" : "\xE2\x80\x94", sizeof(v));
        set_text(w.big, v);
        set_text(w.line1, h.paired ? (h.lights ? h.lights_state : "No lights") : "Open Home to pair");
        set_text(w.line2, h.tv ? h.tv_name : "");
        break;
    }
    case W_MUSIC: {
        const home_summary_t &h = ui.home_data;
        const int vol = h.music && h.volume > 0 ? h.volume : 0;
        card_fill_set(w.card, vol, VIOLET.glow, VIOLET.glow);
        ink(slot, false);
        if (h.music && h.volume >= 0) snprintf(v, sizeof(v), "%d", h.volume);
        else strlcpy(v, "\xE2\x80\x94", sizeof(v));
        set_text(w.big, v);
        set_text(w.line1, h.music ? h.music_state : h.paired ? "No speaker" : "Open Home to pair");
        set_text(w.line2, h.music ? "Volume" : "");
        break;
    }
    case W_SWIFTY: {
        std::string plain;
        for (const char *c = ui.swifty.last; *c; ++c)
            if (*c != '*' && *c != '`' && *c != '#') plain += *c == '\n' ? ' ' : *c;
        set_text(w.line1, plain.empty() ? (ui.swifty.configured ? "Hold the AI key and ask me anything." : "Set me up with mosaico-config.py.")
                                        : plain.c_str());
        set_text(w.title, ui.swifty.listening ? "Listening\xE2\x80\xA6" : ui.swifty.busy ? "Thinking\xE2\x80\xA6" : "Swifty");
        break;
    }
    case W_BATTERY: {
        const int b = ui.battery;
        card_fill_set(w.card, b > 0 ? b : 0, b >= 0 && b <= 20 ? RED : GREEN.glow, b >= 0 && b <= 20 ? RED : GREEN.glow);
        ink(slot, b > 45);
        if (b >= 0) snprintf(v, sizeof(v), "%d%%", b);
        else strlcpy(v, "\xE2\x80\x94", sizeof(v));
        set_text(w.big, v);
        set_text(w.line1, b < 0 ? "No reading" : ui.charging ? "Charging" : "On battery");
        set_text(w.line2, "");
        break;
    }
    default:  // clock
        set_text(w.big, ui.clock_text);
        set_text(w.line1, ui.date_text);
        set_text(w.line2, "");
        break;
    }
}

void close_picker()
{
    if (ui.picker) lv_obj_delete_async(ui.picker);
    ui.picker = nullptr;
}

void pick_cb(lv_event_t *e)
{
    const int v = int(intptr_t(lv_event_get_user_data(e)));
    const int slot = v >> 8, kind = v & 0xFF;
    close_picker();
    ui.w[slot].kind = kind;
    build_widget(slot);
    if (ui.hooks.save_widgets) ui.hooks.save_widgets(ui.w[0].kind, ui.w[1].kind);
}

void open_picker(int slot)
{
    close_picker();
    lv_obj_t *o = box(ui.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    ui.picker = o;
    label(o, slot ? "Right widget" : "Left widget", 0, 26, PANEL, TEXT, 24, true, LV_TEXT_ALIGN_CENTER);
    for (int k = 0; k < W_KINDS; ++k) {
        lv_obj_t *r = glass(o, 50 + (k % 2) * 194, 74 + (k / 2) * 92, 186, 84, 22, CYAN);
        icon(r, WIDGET_ICONS[k], 16, 14, 26, k == ui.w[slot].kind ? CYAN.glow : TEXT, 1.7f);
        label(r, WIDGET_NAMES[k], 16, 48, 160, TEXT, 17, true);
        if (k == ui.w[slot].kind) icon(r, "check", 146, 16, 24, CYAN.glow, 2.0f);
        press_fx(r);
        on(r, pick_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(slot << 8 | k)));
    }
    lv_obj_t *cancel = field_button(o, 170, 366, 140, 46, "Cancel", 16, TEXT2);
    on(cancel, [](lv_event_t *) { close_picker(); });
    stagger(o, 0, 12);
}

// ---- status bar: clock, date, Wi-Fi, battery ---------------------------------------------------
// How far in the rounded corner reaches at height y (the glass's radius).
int corner_inset(int y)
{
    const int r = ui.radius;
    if (r <= 0 || y >= r) return 0;
    const float d = float(r - y);
    return int(ceilf(float(r) - sqrtf(float(r * r) - d * d)));
}

void build_status_bar()
{
    // Out towards the corners, as far as the rounded glass allows at this
    // height: the clock and Wi-Fi on the left, the battery on the right.
    const int edge = std::max(20, corner_inset(14) + 10);
    ui.clock = label(ui.home, "--:--", edge, 12, 60, TEXT, 15, true);
    ui.wifi = label(ui.home, LV_SYMBOL_WIFI, edge + 50, 12, 24, TEXT, 15, true);
    ui.date = label(ui.home, "", 150, 12, 180, TEXT2, 15, true, LV_TEXT_ALIGN_CENTER);
    // The battery: an outline with a fill coloured by charge, and a bolt while charging.
    const int bx = PANEL - edge - 33;
    ui.batt = box(ui.home, bx, 14, 30, 15, 4);
    lv_obj_set_style_border_width(ui.batt, 2, 0);
    lv_obj_set_style_border_color(ui.batt, lv_color_hex(TEXT2), 0);
    ui.batt_fill = box(ui.batt, 1, 1, 20, 9, 2);
    lv_obj_set_style_bg_color(ui.batt_fill, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(ui.batt_fill, LV_OPA_COVER, 0);
    ui.batt_nub = box(ui.home, bx + 31, 19, 3, 6, 1);
    lv_obj_set_style_bg_color(ui.batt_nub, lv_color_hex(TEXT2), 0);
    lv_obj_set_style_bg_opa(ui.batt_nub, LV_OPA_COVER, 0);
    ui.batt_bolt = label(ui.batt, LV_SYMBOL_CHARGE, 0, -2, 26, 0xFFFFFF, 12, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(ui.batt_bolt, LV_OBJ_FLAG_HIDDEN);
}

void update_status_bar()
{
    set_text(ui.clock, ui.clock_text);
    set_text(ui.date, ui.date_text);
    const int b = ui.battery;
    const bool known = b >= 0;
    for (lv_obj_t *o : {ui.batt, ui.batt_nub}) {
        if (known) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (!known) return;
    // No number: the colour says it. Green charging or plenty, amber under
    // half, red under a fifth.
    const int pct = std::clamp(b, 0, 100);
    const uint32_t color = ui.charging ? OK : pct <= 20 ? RED : pct <= 50 ? WARN : OK;
    lv_obj_set_width(ui.batt_fill, std::max(3, 24 * pct / 100));
    lv_obj_set_style_bg_color(ui.batt_fill, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(ui.batt, lv_color_hex(ui.charging ? OK : TEXT2), 0);
    lv_obj_set_style_bg_color(ui.batt_nub, lv_color_hex(ui.charging ? OK : TEXT2), 0);
    if (ui.charging) lv_obj_remove_flag(ui.batt_bolt, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui.batt_bolt, LV_OBJ_FLAG_HIDDEN);
}

// ---- Settings page -----------------------------------------------------------------------
void bright_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_current_target_obj(e);
    const int v = std::max(5, int(lv_slider_get_value(s)));
    const bool release = lv_event_get_code(e) == LV_EVENT_RELEASED;
    bar_set(ui.s_bright_bar, v);
    char text[16];
    snprintf(text, sizeof(text), "%d%%", v);
    set_text(ui.s_bright, text);
    if (ui.hooks.brightness) ui.hooks.brightness(v, release);
}

void haptics_cb(lv_event_t *e)
{
    if (ui.hooks.haptics) ui.hooks.haptics(toggle_on(lv_event_get_current_target_obj(e)));
}

void rotate_cb(lv_event_t *e)
{
    if (ui.hooks.auto_rotate) ui.hooks.auto_rotate(toggle_on(lv_event_get_current_target_obj(e)));
}

void corners_step_cb(lv_event_t *e)
{
    launcher_ui_calibration(true, std::clamp(ui.radius + int(intptr_t(lv_event_get_user_data(e))), MIN_RADIUS, MAX_RADIUS));
}

void build_settings(int brightness, bool haptics, bool auto_rotate)
{
    // A sheet over the home screen, opened from the Settings icon.
    lv_obj_t *c = box(ui.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_add_flag(c, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    ui.settings = c;
    lv_obj_t *back = box(c, 56, 14, 44, 44, 22);
    lv_obj_set_style_bg_color(back, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    icon(back, "back", 10, 10, 24, TEXT, 2.0f);
    press_fx(back);
    on(back, [](lv_event_t *) { lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN); });
    label(c, "Settings", 112, 18, 0, TEXT, 30, true);
    lv_obj_t *br = glass(c, 10, 96, 460, 86, 26, GREEN);
    label(br, "Brightness", 24, 16, 200, TEXT, 18, true);
    char text[16];
    snprintf(text, sizeof(text), "%d%%", brightness);
    ui.s_bright = label(br, text, 312, 17, 124, TEXT2, 16, true, LV_TEXT_ALIGN_RIGHT);
    ui.s_bright_bar = bar(br, 24, 54, 412, 12, brightness, GREEN);
    lv_obj_t *slider = lv_slider_create(br);
    lv_obj_remove_style_all(slider);
    lv_obj_set_pos(slider, 12, 36);
    lv_obj_set_size(slider, 436, 46);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, brightness, LV_ANIM_OFF);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_add_event_cb(slider, bright_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, bright_cb, LV_EVENT_RELEASED, nullptr);

    lv_obj_t *hp = glass(c, 10, 192, 224, 112, 26, GREEN);
    // Two switches: touch ticks, and turning the UI with the board.
    label(hp, "Haptics", 22, 19, 120, TEXT, 17, true);
    lv_obj_t *t = toggle(hp, 146, 14, haptics, GREEN);
    lv_obj_add_event_cb(t, haptics_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    label(hp, "Auto-rotate", 22, 67, 120, TEXT, 17, true);
    t = toggle(hp, 146, 62, auto_rotate, GREEN);
    lv_obj_add_event_cb(t, rotate_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *cr = glass(c, 246, 192, 224, 112, 26, GREEN);
    label(cr, "Corners", 22, 16, 170, TEXT, 18, true);
    label(cr, "Glass radius", 22, 42, 170, TEXT3, 14);
    lv_obj_t *minus = field_button(cr, 20, 68, 48, 34, "\xE2\x88\x92", 20);
    on(minus, corners_step_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(-4)));
    snprintf(text, sizeof(text), "%d", ui.radius);
    ui.s_radius = label(cr, text, 68, 69, 88, TEXT, 24, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *plus = field_button(cr, 156, 68, 48, 34, "+", 20);
    on(plus, corners_step_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(4)));

    lv_obj_t *wf = glass(c, 10, 316, 460, 112, 28, GREEN);
    ui.s_wifi_dot = dot(wf, 26, 24, 10, OFF, false);
    ui.s_ssid = label(wf, "Wi-Fi", 46, 16, 230, TEXT, 18, true);
    ui.s_wifi_state = label(wf, "", 286, 18, 150, TEXT2, 15, true, LV_TEXT_ALIGN_RIGHT);
    ui.s_detail = label(wf, "", 26, 48, 410, TEXT2, 15);
    ui.s_versions = label(wf, "", 26, 74, 380, TEXT3, 14);

    // Time zone: automatic (from the network's location) or one of the clock's zones.
    lv_obj_t *tz = glass(c, 10, 440, 460, 112, 28, GREEN);
    label(tz, "Time zone", 24, 16, 200, TEXT, 18, true);
    label(tz, "Clocks in every app", 24, 42, 220, TEXT3, 14);
    ui.s_zone = label(tz, "", 24, 72, 250, TEXT2, 16, true);
    lv_obj_t *prev = field_button(tz, 296, 56, 64, 40, "", 16);
    icon(prev, "back", 20, 8, 24, TEXT, 2.0f);
    on(prev, [](lv_event_t *) { if (ui.hooks.time_zone) ui.hooks.time_zone(-1); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *next = field_button(tz, 372, 56, 64, 40, "", 16);
    icon(next, "chev", 20, 8, 24, TEXT, 2.0f);
    on(next, [](lv_event_t *) { if (ui.hooks.time_zone) ui.hooks.time_zone(1); }, LV_EVENT_CLICKED, nullptr);
    // Camera: front/rear found automatically, or only by its button.
    lv_obj_t *cam = glass(c, 10, 564, 460, 96, 28, GREEN);
    label(cam, "Camera", 24, 16, 260, TEXT, 18, true);
    label(cam, "Find front or rear automatically", 24, 44, 300, TEXT3, 14);
    label(cam, "Off: only the camera's button switches", 24, 64, 300, TEXT3, 14);
    ui.s_cam_auto = toggle(cam, 360, 28, true, GREEN);
    lv_obj_add_event_cb(ui.s_cam_auto, [](lv_event_t *e) {
        if (ui.hooks.camera_auto) ui.hooks.camera_auto(toggle_on(lv_event_get_current_target_obj(e)));
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    box(c, 0, 660, 1, 60);  // room to scroll the last card clear of the rounded bottom
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
}

void update_settings(const LauncherStatus &s)
{
    const uint32_t color = s.wifi_connected ? OK : s.wifi_configured ? WARN : RED;
    lv_obj_set_style_bg_color(ui.s_wifi_dot, lv_color_hex(color), 0);
    set_text(ui.s_ssid, s.ssid && *s.ssid ? s.ssid : "Wi-Fi");
    set_text(ui.s_wifi_state, s.wifi_connected ? "Connected" : s.wifi_configured ? "Connecting\xE2\x80\xA6" : "Not set");
    set_color(ui.s_wifi_state, color);
    set_text(ui.s_detail, s.wifi_connected && s.ip ? s.ip : s.wifi_configured ? "Joining the network\xE2\x80\xA6"
                                                                            : "Set Wi-Fi in Vibe Mode");
    set_text(ui.s_versions, s.versions ? s.versions : "");
    set_text(ui.s_zone, s.zone ? s.zone : "UTC");
    if (ui.s_cam_auto && toggle_on(ui.s_cam_auto) != s.camera_auto) toggle_set(ui.s_cam_auto, s.camera_auto, false);
}

// ---- corner calibration --------------------------------------------------------------------
void calibration_refresh()
{
    lv_obj_set_style_radius(ui.guide, ui.draft, 0);
    char value[16];
    snprintf(value, sizeof(value), "%d", ui.draft);
    lv_label_set_text(ui.cal_value, value);
}

void step_cb(lv_event_t *e)
{
    ui.draft = std::clamp(ui.draft + int(intptr_t(lv_event_get_user_data(e))), MIN_RADIUS, MAX_RADIUS);
    calibration_refresh();
}

void save_cb(lv_event_t *)
{
    if (ui.hooks.save_radius) ui.hooks.save_radius(ui.draft);
}

void cancel_cb(lv_event_t *) { launcher_ui_calibration(false, ui.radius); }
}  // namespace

lv_obj_t *launcher_ui_build(const LauncherHooks &hooks, int radius, int brightness, bool haptics, bool auto_rotate,
                            const lv_image_dsc_t *portrait, int left_widget, int right_widget)
{
    (void)portrait;
    ui.hooks = hooks;
    ui.radius = ui.draft = radius;
    lv_obj_t *s = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    ui.screen = s;
    backdrop(s, {&SKY, true, 540, 220, 80, 60});  // the wallpaper
    ui.home = box(s, 0, 0, PANEL, PANEL);
    build_status_bar();
    ui.w[0].kind = left_widget >= 0 && left_widget < W_KINDS ? left_widget : W_GLIMDOCK;
    ui.w[1].kind = right_widget >= 0 && right_widget < W_KINDS ? right_widget : W_LIGHTS;
    build_widget(0);
    build_widget(1);
    build_grid();
    build_settings(brightness, haptics, auto_rotate);
    stagger(ui.grid, 0, 12);
    return s;
}

void launcher_ui_update(const LauncherStatus &status, const glimdock_summary_t &glim, const home_summary_t &home,
                        const camera_summary_t &camera, const swifty_summary_t &swifty, const files_summary_t &files)
{
    (void)camera;
    (void)files;
    strlcpy(ui.clock_text, status.clock ? status.clock : "--:--", sizeof(ui.clock_text));
    strlcpy(ui.date_text, status.date ? status.date : "", sizeof(ui.date_text));
    ui.battery = status.battery_pct;
    ui.charging = status.charging;
    ui.glim = glim;
    ui.home_data = home;
    ui.swifty = swifty;
    ui.have_data = true;
    set_color(ui.wifi, status.wifi_connected ? TEXT : status.wifi_configured ? WARN : RED);
    update_status_bar();
    for (int i = 0; i < 2; ++i) update_widget(i);
    if (glim.ready && glim.alerts > 0) {
        char v[8];
        snprintf(v, sizeof(v), "%d", std::min(glim.alerts, 99));
        set_text(ui.badge_text, v);
        lv_obj_remove_flag(ui.badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.badge, LV_OBJ_FLAG_HIDDEN);
    }
    if (swifty.listening || swifty.busy) lv_obj_remove_flag(ui.swifty_dot, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui.swifty_dot, LV_OBJ_FLAG_HIDDEN);
    update_settings(status);
}

// Back home: the icons settle in again, as a phone's do.
void launcher_ui_entered(ShellApp app)
{
    (void)app;
    lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
    close_picker();
    stagger(ui.grid, 0, 10);
}

// 0-5: the home screen; 6: Settings (kept for the preview).
void launcher_ui_show_page(int index)
{
    if (index >= 6) open_app(-1);
    else lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
}

void launcher_ui_calibration(bool visible, int radius)
{
    if (ui.calibration) {
        lv_obj_delete(ui.calibration);
        ui.calibration = ui.guide = ui.cal_value = nullptr;
    }
    if (!visible) return;
    ui.draft = radius;
    lv_obj_t *c = box(ui.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);  // swallow taps meant for the pages
    ui.calibration = c;
    backdrop(c, {&CYAN, true});
    ui.guide = box(c, 0, 0, PANEL, PANEL, radius);
    lv_obj_set_style_border_width(ui.guide, 3, 0);
    lv_obj_set_style_border_color(ui.guide, lv_color_hex(CYAN.hi), 0);
    lv_obj_t *content = box(c, 0, 0, PANEL, PANEL);
    label(content, "Screen corners", 40, 92, 400, TEXT, 30, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *hint = label(content, "Adjust until the cyan outline just meets the edge of the glass.", 80, 136, 320,
                           TEXT2, 16, false, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(hint, LV_SIZE_CONTENT);
    lv_obj_t *minus = field_button(content, 96, 210, 80, 64, "\xE2\x88\x92", 30);
    lv_obj_add_event_cb(minus, step_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(-4)));
    ui.cal_value = label(content, "", 176, 214, 128, TEXT, 46, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *plus = field_button(content, 304, 210, 80, 64, "+", 30);
    lv_obj_add_event_cb(plus, step_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(4)));
    lv_obj_t *cancel = field_button(content, 96, 318, 136, 50, "Cancel", 17, TEXT2);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *save = fill_button(content, 248, 318, 136, 50, "Save", CYAN, 17);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, nullptr);
    calibration_refresh();
    stagger(content, 0, 15);
}
