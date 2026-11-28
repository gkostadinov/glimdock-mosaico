// SPDX-License-Identifier: Apache-2.0
// Swifty Home wall display: the hub web app's Home view, ported from the
// Mosaico Lua app (main.lua, sw.lua). Layout is in 320-unit squares scaled to
// the frame. Pages are rebuilt from the model when data changes, never while a
// finger is down; drag sliders update their own fill live.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <functional>
#include <map>
#include <memory>

#include "display_clock.h"
#include "home_app.h"
#include "home_icons.h"
#include "home_model.h"
#include "home_prefs.h"
#include "mui.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define HOME_LOG(...) ESP_LOGI("home", __VA_ARGS__)
#else
#define HOME_LOG(...) (printf(__VA_ARGS__), printf("\n"))
#endif

namespace home {
namespace {

// ---- theme (theme.lua: web tokens pre-blended for solid LVGL fills) --------
uint32_t mix(uint32_t fg, uint32_t bg, double a)
{
    auto ch = [&](int shift) {
        return uint32_t(floor(((fg >> shift) & 255) * a + ((bg >> shift) & 255) * (1 - a) + 0.5)) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

uint32_t hsl(double h, double s, double l)
{
    h = fmod(fmod(h, 360) + 360, 360) / 360;
    s /= 100;
    l /= 100;
    auto f = [&](double n) {
        double k = fmod(n + h * 12, 12), a = s * std::min(l, 1 - l);
        return l - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
    };
    return uint32_t(floor(f(0) * 255 + 0.5)) << 16 | uint32_t(floor(f(8) * 255 + 0.5)) << 8 |
           uint32_t(floor(f(4) * 255 + 0.5));
}

struct Palette {
    uint32_t bg, text, text2, text3, tint, tint_soft, on_tint, fill_a, fill_b, card, card_edge, line, field, tint_wash,
             amber, red, ok, knob, toggle_off, dot_off, unlit;
    double lit_l;
};
constexpr uint32_t LIGHT_BG = 0xF4F1F7, DARK_BG = 0x121019;
Palette light_palette()
{
    return {LIGHT_BG, 0x15121D, mix(0x15121D, LIGHT_BG, 0.74), mix(0x15121D, LIGHT_BG, 0.68), 0x6644DD, 0x7A5BF5,
            0xFFFFFF, 0x5E3DD0, 0x4A2DB0, 0xFFFFFF, mix(0x301E50, 0xFFFFFF, 0.14), mix(0x15121D, 0xFFFFFF, 0.10),
            mix(0x15121D, 0xFFFFFF, 0.06), mix(0x6644DD, 0xFFFFFF, 0.10), 0x9A6210, 0xB5302A, 0x2E7D4F, 0xFFFFFF,
            0x5D5A64, 0x8A8A90, 0xFFFFFF, 80};
}
// AMOLED, after Vibe Mode: true black, flat near-black cards (see mui.h).
Palette dark_palette()
{
    return {0x000000, mui::TEXT, mui::TEXT2, mui::TEXT3, 0x7A5BF5, 0x8C73FF, 0xFFFFFF, 0x7A5BF5, 0x7A5BF5, mui::CARD,
            mui::EDGE, 0x222226, mui::FIELD, 0x1C1530, 0xF0B858, 0xFF8078, 0x7AD6A0, 0xFFFFFF, mui::OFF, mui::OFF,
            mui::CARD, 72};
}
Palette c = dark_palette();
bool light = false;

constexpr float VIEW = 320, PAD = 10;
float VIEW_H = VIEW;  // the frame is taller than wide: it fills the panel's height
float S = 1.35f;
int P(float v)
{
    return int(lroundf(v * S));
}

const lv_font_t *font(float size, bool semi)
{
    return mui::font(P(size), semi);
}

// ---- app state ---------------------------------------------------------------
enum class Page { Pair, Edit, Home, Lights, Music, Pets, Confirm, Settings };
struct EditState {
    std::string title, text, mode = "lower";
    bool upper_only = false;
    size_t limit = 256;
    std::function<void(const std::string &)> done;
};
struct {
    lv_obj_t *root = nullptr;
    bool built = false, active = false, dirty = true;
    Page page = Page::Home, rendered = Page::Home;
    bool has_rendered = false;
    DocPtr prep;
    std::string prep_name, prep_target, noresp, last_scene, pending_pets;
    std::map<std::string, int> drag;
    struct {
        std::string text, kind;
        uint32_t at = 0;
        bool shown = false;
    } toast;
    lv_obj_t *scroller = nullptr, *backdrop = nullptr;
    int32_t saved_scroll = 0, room_row_x = 0;
    std::string room_row_for;  // the room the row was last scrolled to
    bool confirm_unpair = false;
    uint32_t last_render = 0, last_revision = 0;
    void (*exit_to_launcher)() = nullptr;
    // Pairing: the hub address is typed in (or set over ESP-Iris), no default.
    struct {
        std::string hub, code, message;
        bool ok = false, busy = false;
    } pair;
    std::unique_ptr<EditState> edit;
    Derived d;
} G;

void dirty()
{
    G.dirty = true;
}
void open(Page page)
{
    G.page = page;
    G.dirty = true;
}

// ---- callbacks owned by objects ------------------------------------------------
struct Action {
    std::function<void()> fn;
};
void free_action(lv_event_t *e)
{
    delete static_cast<Action *>(lv_event_get_user_data(e));
}
void run_action(lv_event_t *e)
{
    static_cast<Action *>(lv_event_get_user_data(e))->fn();
}
void click(lv_obj_t *o, std::function<void()> fn)
{
    auto *a = new Action{std::move(fn)};
    lv_obj_add_flag(o, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_event_cb(o, run_action, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(o, free_action, LV_EVENT_DELETE, a);
}

// ---- widgets (sw.lua) --------------------------------------------------------------
struct BoxOpts {
    int64_t bg = -1;
    lv_opa_t bg_opa = LV_OPA_COVER;
    int64_t border = -1;
    float bw = -1;
    float radius = 15;
    lv_opa_t opa = LV_OPA_COVER;
    bool hidden = false;
    int scroll = 0;  // 1 vertical, 2 horizontal
    bool glass = false, fill = false, field = false, glow = true;  // AMOLED looks
};

lv_obj_t *box(lv_obj_t *parent, float x, float y, float w, float h, BoxOpts o = {})
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, P(x), P(y));
    lv_obj_set_size(b, std::max(1, P(w)), std::max(1, P(h)));
    lv_obj_remove_flag(b, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    const uint32_t bg = o.bg < 0 ? c.card : uint32_t(o.bg);
    float bw = o.bw;
    if (bw < 0) {
        bw = (bg != c.bg && o.bg_opa != 0) ? 1 : 0;    // firmware box(): 1 px edge off-page
    }
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, o.bg_opa, 0);
    lv_obj_set_style_border_width(b, bw <= 1 ? int(bw) : P(bw), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(o.border < 0 ? c.card_edge : uint32_t(o.border)), 0);
    lv_obj_set_style_radius(b, P(o.radius), 0);
    lv_obj_set_style_opa(b, o.opa, 0);
    if (o.scroll) {
        lv_obj_add_flag(b, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
        lv_obj_set_scroll_dir(b, o.scroll == 1 ? LV_DIR_VER : LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(b, LV_SCROLLBAR_MODE_AUTO);
    }
    if (o.fill) {
        mui::style_fill(b, mui::VIOLET, o.glow && !light);
    } else if (!light && o.glass) {
        mui::style_glass(b, mui::VIOLET);
    } else if (!light && o.field) {
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, 20, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_color(b, lv_color_white(), 0);
        lv_obj_set_style_border_opa(b, 22, 0);
    }
    if (o.hidden) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
    return b;
}

// Tap feedback for anything that acts: squash and spring back.
lv_obj_t *pressable(lv_obj_t *o)
{
    mui::press_fx(o);
    return o;
}

lv_obj_t *plain(lv_obj_t *parent, float x, float y, float w, float h, int scroll = 0)
{
    BoxOpts o;
    o.bg = c.bg;
    o.bg_opa = LV_OPA_TRANSP;
    o.bw = 0;
    o.radius = 0;
    o.scroll = scroll;
    return box(parent, x, y, w, h, o);
}

BoxOpts card_opts(float radius = 15)
{
    BoxOpts o;
    o.radius = radius;
    o.glass = true;
    return o;
}
lv_obj_t *card(lv_obj_t *parent, float x, float y, float w, float h, float radius = 15)
{
    return box(parent, x, y, w, h, card_opts(radius));
}

struct TextOpts {
    bool semi = false, wrap = false, hidden = false;
    lv_text_align_t align = LV_TEXT_ALIGN_LEFT;
};
lv_obj_t *text(lv_obj_t *parent, const std::string &str, float x, float y, float w, uint32_t color, float size,
               TextOpts o = {})
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, str.c_str());
    lv_obj_set_pos(l, P(x), P(y));
    lv_obj_set_width(l, std::max(1, P(w)));
    const lv_font_t *f = font(size, o.semi);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, o.align, 0);
    if (o.wrap) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    } else {
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(l, lv_font_get_line_height(f));
    }
    if (o.hidden) {
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    }
    return l;
}
TextOpts semi(lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    TextOpts o;
    o.semi = true;
    o.align = align;
    return o;
}
TextOpts wrapped()
{
    TextOpts o;
    o.wrap = true;
    return o;
}

float text_w(const std::string &str, float size, bool is_semi = false)
{
    lv_point_t p;
    lv_text_get_size(&p, str.c_str(), font(size, is_semi), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return float(p.x) / S;
}
float fit(const std::string &str, float w, float size = 12, bool is_semi = false, float min = 8)
{
    while (size > min && text_w(str, size, is_semi) > w) {
        size -= 1;
    }
    return size;
}
float text_h(const std::string &str, float w, float size)
{
    lv_point_t p;
    lv_text_get_size(&p, str.c_str(), font(size, false), 0, 0, P(w), LV_TEXT_FLAG_NONE);
    return float(p.y) / S;
}

void draw_icon(lv_event_t *e)
{
    auto *icon = static_cast<const home_icons::Icon *>(lv_event_get_user_data(e));
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    mui::draw_icon_mask(lv_event_get_layer(e), a, icon, lv_obj_get_style_line_color(o, LV_PART_MAIN),
                        lv_obj_get_style_line_width(o, LV_PART_MAIN));
}

lv_obj_t *icon(lv_obj_t *parent, const char *name, float x, float y, float size, uint32_t color, float width = 1.2f)
{
    lv_obj_t *o = plain(parent, x, y, size, size);
    const home_icons::Icon *found = home_icons::find(name);
    if (!found) {
        return o;
    }
    lv_obj_set_style_line_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(o, std::max(1, int(lroundf(width * S))), 0);
    lv_obj_add_event_cb(o, draw_icon, LV_EVENT_DRAW_MAIN, const_cast<home_icons::Icon *>(found));
    return o;
}

lv_obj_t *dot(lv_obj_t *parent, float x, float y, float size, uint32_t color)
{
    return mui::dot(parent, P(x), P(y), P(size), color, !light && color != c.dot_off);
}

// A drawn level (bulb brightness, volume).
void glow_fill(lv_obj_t *fill, uint32_t color)
{
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);  // flat, as Vibe Mode
}

// Web .toggle (44x26 units): springs and glows when on (mui::toggle).
struct Change {
    std::function<void()> fn;
};
void run_change(lv_event_t *e)
{
    static_cast<Change *>(lv_event_get_user_data(e))->fn();
}
void free_change(lv_event_t *e)
{
    delete static_cast<Change *>(lv_event_get_user_data(e));
}
void toggle(lv_obj_t *parent, float x, float y, bool on, std::function<void()> fn, bool disabled)
{
    lv_obj_t *t = mui::toggle(parent, P(x), P(y), on, mui::VIOLET, float(P(26)) / 32.0f);
    if (light && !on) {
        lv_obj_set_style_bg_color(t, lv_color_hex(c.toggle_off), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(t, LV_OPA_COVER, 0);
    }
    if (disabled) {
        lv_obj_set_style_opa(t, 110, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    auto *ch = new Change{std::move(fn)};
    lv_obj_add_event_cb(t, run_change, LV_EVENT_VALUE_CHANGED, ch);
    lv_obj_add_event_cb(t, free_change, LV_EVENT_DELETE, ch);
}

struct RoundOpts {
    bool active = false, disabled = false;
    int64_t color = -1;
};
void round_button(lv_obj_t *parent, float x, float y, float size, const char *name, std::function<void()> fn, RoundOpts r = {})
{
    BoxOpts o;
    o.bg = r.active ? c.field : c.card;
    o.border = r.active ? c.field : c.card_edge;
    o.bw = r.active ? 0 : 1;
    o.radius = size;
    o.opa = r.disabled ? 110 : 255;
    o.fill = r.active;
    o.field = !r.active;
    lv_obj_t *b = box(parent, x, y, size, size, o);
    const float is = size * 0.5f;
    icon(b, name, (size - is) / 2, (size - is) / 2, is, r.active ? c.on_tint : r.color < 0 ? c.text : uint32_t(r.color), 1.3f);
    if (fn && !r.disabled) {
        click(pressable(b), std::move(fn));
    }
}

struct PillOpts {
    bool on = false, disabled = false;
    float size = 12;
    int64_t color = -1;
};
void pill(lv_obj_t *parent, float x, float y, float w, float h, const std::string &label, std::function<void()> fn,
          PillOpts p = {})
{
    BoxOpts o;
    o.bg = p.on ? c.tint : c.card;
    o.border = p.on ? c.tint : c.card_edge;
    o.bw = p.on ? 0 : 1;
    o.radius = h;
    o.opa = p.disabled ? 110 : 255;
    o.fill = p.on;
    o.field = !p.on;
    lv_obj_t *b = box(parent, x, y, w, h, o);
    const float size = fit(label, w - 12, p.size, true);
    text(b, label, 4, (h - size * 1.25f) / 2, w - 8, p.on ? c.on_tint : (p.color < 0 ? c.text : uint32_t(p.color)), size,
         semi(LV_TEXT_ALIGN_CENTER));
    if (fn && !p.disabled) {
        click(pressable(b), std::move(fn));
    }
}

void heading(lv_obj_t *root, const std::string &title, std::function<void()> back)
{
    lv_obj_t *b = plain(root, 0, 0, 44, 40);
    icon(b, "back", 12, 10, 20, c.tint_soft, 1.8f);
    click(b, std::move(back));
    text(root, title, 42, 10, VIEW - 52, c.text, 16, semi());
}

lv_obj_t *scroller(lv_obj_t *root, float y)
{
    G.scroller = plain(root, 0, y, VIEW, VIEW_H - y, 1);
    return G.scroller;
}

// An lv_slider used only for its drag gesture: invisible, over a drawn fill.
struct Drag {
    std::function<void(int, bool)> fn;
};
void drag_event(lv_event_t *e)
{
    auto *d = static_cast<Drag *>(lv_event_get_user_data(e));
    lv_obj_t *s = lv_event_get_current_target_obj(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        d->fn(lv_slider_get_value(s), false);
    } else if (code == LV_EVENT_RELEASED) {
        d->fn(lv_slider_get_value(s), true);
    } else if (code == LV_EVENT_DELETE) {
        delete d;
    }
}
void drag_slider(lv_obj_t *parent, float x, float y, float w, float h, int value, std::function<void(int, bool)> fn)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_pos(s, P(x), P(y));
    lv_obj_set_size(s, P(w), P(h));
    lv_obj_add_flag(s, LV_OBJ_FLAG_PRESS_LOCK);
    lv_slider_set_range(s, 0, 100);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_opa(s, LV_OPA_TRANSP, 0);
    auto *d = new Drag{std::move(fn)};
    lv_obj_add_event_cb(s, drag_event, LV_EVENT_ALL, d);
}

// ---- feedback and actions ---------------------------------------------------------
void toast(const std::string &message, const char *kind = "ok")
{
    G.toast.text = message;
    G.toast.kind = kind;
    G.toast.at = now_ms();
    G.toast.shown = true;
    dirty();
}

// The value the person set (lean) stays on screen while the command runs,
// then until the device agrees; a failure drops it at once (web actions.js).
RunHandler after_run(const std::string &name, const std::string &target = std::string())
{
    return [name, target](const DocPtr & run, const std::string & error) {
        if (!run) {
            if (!target.empty()) {
                lean_settle(target, false);
            }
            toast(error.empty() ? "That did not work." : error, "fail");
            dirty();
            return;
        }
        const std::string phase = (*run)["phase"] | "";
        if (phase == "commanded") {
            return;    // still running: keep holding
        }
        const bool failed = phase == "no_response" || phase == "failed" || phase == "refused";
        if (!target.empty()) {
            lean_settle(target, !failed);
        }
        if (failed) {
            G.noresp = name;
            toast("The " + name + " didn't answer.", "fail");
        } else if (phase == "simulated") {
            toast("Dry-run: nothing was sent.", "ok");
        } else if (phase == "confirmed" || phase == "sent") {
            G.noresp.clear();
        }
        dirty();
    };
}

// Read-only for the profile signed in on the board (home_app_set_read_only):
// pages show, actions are refused with a note.
bool read_only = false;
const char *READ_ONLY_NOTE = "Read-only for this profile";

// Read-only: says so and refuses (true).
bool refused_read_only()
{
    if (read_only) {
        toast(READ_ONLY_NOTE, "fail");
    }
    return read_only;
}

// Runs an action by its catalog flow: direct, or prepare -> confirm page.
void act(const std::string &action, const std::string &target, JsonDocument params, const std::string &label,
         const std::string &name = std::string())
{
    if (refused_read_only()) {
        lean_settle(target, false);
        return;
    }
    const std::string flow = hub.flow(action);
    const std::string who = name.empty() ? label : name;
    if (!hub.can(action)) {
        lean_settle(target, false);
        toast("This display may not do that.", "fail");
        return;
    }
    if (flow == "prepare_confirm") {
        hub.prepare(action, target, std::move(params), [who, target](const DocPtr & prep, const std::string & error) {
            if (!prep) {
                lean_settle(target, false);
                toast(error, "fail");
                return;
            }
            G.prep = prep;
            G.prep_name = who;
            G.prep_target = target;
            open(Page::Confirm);
        });
    } else if (flow == "direct") {
        hub.execute(action, target, std::move(params), after_run(who, target));
    } else {
        lean_settle(target, false);
        toast("That needs a passkey. Use your phone.", "fail");
    }
}

JsonDocument param(const char *key, bool value)
{
    JsonDocument d;
    d[key] = value;
    return d;
}
JsonDocument param(const char *key, int value)
{
    JsonDocument d;
    d[key] = value;
    return d;
}
JsonDocument param(const char *key, const std::string &value)
{
    JsonDocument d;
    d[key] = value;
    return d;
}
JsonDocument no_params()
{
    JsonDocument d;
    d.to<JsonObject>();
    return d;
}

// ---- rooms ---------------------------------------------------------------------------
JsonArrayConst rooms()
{
    return hub.rooms ? (*hub.rooms)["rooms"].as<JsonArrayConst>() : JsonArrayConst();
}
JsonObjectConst room_of(const std::string &target)
{
    for (JsonObjectConst r : rooms())
        for (JsonObjectConst d : r["devices"].as<JsonArrayConst>())
            if (target == (d["target"] | "")) {
                return r;
            }
    return JsonObjectConst();
}
JsonObjectConst picked_room()
{
    if (hub.prefs.room.empty()) {
        return JsonObjectConst();
    }
    for (JsonObjectConst r : rooms()) if (hub.prefs.room == (r["id"] | "")) {
            return r;
        }
    return JsonObjectConst();
}
bool in_view(const std::string &target)
{
    JsonObjectConst here = picked_room();
    if (here.isNull() || target.empty()) {
        return true;
    }
    JsonObjectConst r = room_of(target);
    return r.isNull() || strcmp(r["id"] | "", here["id"] | "") == 0;
}
// A room is offered only if one of its devices is visible to this display.
bool room_has_devices(JsonObjectConst room)
{
    if (!hub.devices) {
        return true;
    }
    for (JsonObjectConst d : room["devices"].as<JsonArrayConst>()) {
        const char *t = d["target"] | "";
        for (JsonObjectConst dev : (*hub.devices)["devices"].as<JsonArrayConst>())
            if (!strcmp(dev["target"] | "", t)) {
                return true;
            }
    }
    return false;
}
std::vector<Item> room_bulbs(JsonObjectConst room)
{
    if (room.isNull()) {
        return G.d.bulbs;
    }
    std::vector<Item> list;
    for (const Item &b : G.d.bulbs) {
        JsonObjectConst r = room_of(b.target);
        if (!r.isNull() && strcmp(r["id"] | "", room["id"] | "") == 0) {
            list.push_back(b);
        }
    }
    return list;
}

int clamp_pct(double v)
{
    return int(std::max(0.0, std::min(100.0, floor(v + 0.5))));
}

// ---- home page ------------------------------------------------------------------------
// Lights / Music tile with drag-to-set (an invisible slider over the card).
void tile(lv_obj_t *root, float x, float y, float w, float h, bool lights)
{
    JsonObjectConst room = picked_room();
    double p = 0;
    int64_t fill = -1;
    std::string title, state, big;
    const char *icon_name;
    const std::string key = lights ? "lights" : "music";
    const bool dragging = G.drag.count(key) != 0;
    if (lights) {
        LightsSummary s = lights_summary(room_bulbs(room));
        p = dragging ? G.drag[key] : (s.n > 0 ? s.avg : 0);
        if (s.n > 0) {
            fill = hsl(s.hue, 85, c.lit_l);
        }
        if (dragging) {
            fill = hsl(s.n > 0 ? s.hue : 34, 85, c.lit_l);
        }
        title = room.isNull() ? "Lights" : std::string(room["name"] | "Lights");
        state = s.state;
        big = (dragging || s.n > 0) ? std::to_string(clamp_pct(p)) + "%" : "";
        icon_name = "bulb";
    } else {
        const auto &sp = G.d.speaker;
        const auto &tv = G.d.tv;
        p = dragging ? G.drag[key] : (sp && sp->known ? sp->vol : 0);
        fill = c.fill_a;
        title = (sp && tv && w >= 150) ? "Music & TV" : (tv && !sp) ? "TV" : "Music";
        std::vector<std::string> words;
        if (tv) {
            words.push_back(tv->known ? (tv->on ? "TV on" : "TV off") : "TV not reporting");
        } else if (sp && !sp->known) {
            words.push_back("Not reporting");
        } else if (sp) {
            words.push_back("Speaker");
        }
        if (sp && sp->known && sp->mute) {
            words.push_back(tv ? "muted" : "Muted");
        }
        for (size_t i = 0; i < words.size(); ++i) {
            state += (i ? " \xC2\xB7 " : "") + words[i];
        }
        big = (sp && sp->known) ? std::to_string(clamp_pct(p)) : "\xE2\x80\x94";
        icon_name = "speaker";
    }
    lv_obj_t *cardo = card(root, x, y, w, h, 16);
    const float fw = fill >= 0 ? std::max(0.0, w * p / 100) : 0;
    // Two-tone vertical gradient of the fill colour, clipped to the card.
    auto shade = [lights](uint32_t base, bool top) {
        if (!lights) {
            return top ? c.fill_a : c.fill_b;
        }
        return top ? mix(0xFFFFFF, base, 0.34) : base;
    };
    const uint32_t base = fill >= 0 ? uint32_t(fill) : c.fill_a;
    mui::card_fill(cardo, fill >= 0 ? clamp_pct(p) : 0, shade(base, true), shade(base, false));
    const bool covered_left = fw > 40, covered_right = fw > w - 50;
    const uint32_t fg = lights ? 0x2A1A05 : c.on_tint;
    icon(cardo, icon_name, 12, 12, 16, covered_left ? fg : c.text, 1.4f);
    text(cardo, title, 34, 12, w - 34 - 40, covered_left ? fg : c.text, fit(title, w - 74, 12, true), semi());
    // The state ends before the big number (right-aligned at w - 12): on a
    // narrow tile "TV not reporting" ran under the volume.
    const float state_w = std::min(w * 0.6f, w - 12 - text_w(big, 32, true) - 8 - 12);
    text(cardo, state, 12, h - 24, std::max(20.0f, state_w), covered_left ? fg : c.text2, 10);
    lv_obj_t *big_label = text(cardo, big, w - 110, h - 50, 98, covered_right ? fg : c.text, 32, TextOpts{true, false, false, LV_TEXT_ALIGN_RIGHT});
    const uint32_t live_fill = fill >= 0 ? uint32_t(fill) : uint32_t(c.fill_a);
    drag_slider(cardo, 0, 0, w, h, clamp_pct(p), [ = ](int v, bool released) {
        G.drag[key] = v;
        // Live feedback without rebuilding under the finger.
        const uint32_t live = lights && fill < 0 ? hsl(34, 85, c.lit_l) : live_fill;
        mui::card_fill_set(cardo, v, shade(live, true), shade(live, false));
        lv_label_set_text(big_label, (std::to_string(v) + (lights ? "%" : "")).c_str());
        if (!released) {
            return;
        }
        G.drag.erase(key);
        dirty();
        if (lights) {
            JsonObjectConst here = picked_room();
            for (const Item &b : room_bulbs(here)) {
                lean(b.target, "on", v > 0);
                if (v > 0) {
                    lean(b.target, "bri", v);
                }
            }
            if (!here.isNull()) {
                const std::string id = here["id"] | "";
                if (v == 0) {
                    act("room.power", id, param("on", false), "lights");
                } else {
                    act("room.brightness", id, param("percent", v), "lights");
                }
            } else if (v == 0) {
                act("lights.power", "all_bulbs", param("on", false), "lights");
            } else {
                act("lights.brightness", "all_bulbs", param("percent", std::max(1, v)), "lights");
            }
        } else if (G.d.speaker) {
            lean(G.d.speaker->target, "vol", v);
            act("speaker.volume", G.d.speaker->target, param("level", v), "speaker");
        }
    });
    lv_obj_t *open_area = plain(cardo, w - 40, 0, 40, 36);
    icon(open_area, "chev", 18, 10, 14, fw > w - 40 ? fg : c.text3, 1.4f);
    click(open_area, [lights] { open(lights ? Page::Lights : Page::Music); });
}

// web ui/scenes.js: the house's scenes on All; in a room, its level (Bright
// 100%, Relax 35%, Off), plus Movie in the TV's room. Hidden where there are
// no lights.
float scenes_bar(lv_obj_t *root, float y)
{
    JsonObjectConst room = picked_room();
    struct Scene {
        const char *label, *action, *target;
        int level;
    };
    static const Scene all[] = {{"Movie", "scene.movie", "movie", -1}, {"Evening", "scene.apply", "evening", -1},
        {"Lights off", "scene.apply", "all_off", -1}
    };
    static const Scene in_room[] = {{"Bright", "room.brightness", "", 100}, {"Relax", "room.brightness", "", 35},
        {"Off", "room.power", "", 0}, {"Movie", "scene.movie", "movie", -1}
    };
    const std::string room_id = room.isNull() ? "" : std::string(room["id"] | "");
    const std::string room_name = room.isNull() ? "" : std::string(room["name"] | "");
    const bool tv_here = !room.isNull() && G.d.tv && room_id == (room_of(G.d.tv->target)["id"] | "");
    const Scene *names = room.isNull() ? all : in_room;
    const int n = room.isNull() ? 3 : tv_here ? 4 : 3;
    const float w = VIEW - 2 * PAD;
    const char *labels[4];
    int selected = -1;
    for (int i = 0; i < n; ++i) {
        labels[i] = names[i].label;
        if (G.last_scene == room_id + "/" + names[i].label) {
            selected = i;
        }
    }
    lv_obj_t *bar = mui::capsule(root, P(PAD), P(y), P(w), P(40), labels, n, selected, mui::VIOLET, P(12));
    auto *ch = new Change{[bar, names, room_id, room_name] {
            const Scene s = names[mui::capsule_selected(bar)];
            G.last_scene = room_id + "/" + s.label;
            if (s.level > 0) {
                act(s.action, room_id, param("percent", s.level), room_name + " " + s.label, "lights");
            } else if (s.level == 0) {
                act(s.action, room_id, param("on", false), room_name + " " + s.label, "lights");
            } else
            {
                act(s.action, s.target, no_params(), s.label, "lights");
            }
        }};
    lv_obj_add_event_cb(bar, run_change, LV_EVENT_VALUE_CHANGED, ch);
    lv_obj_add_event_cb(bar, free_change, LV_EVENT_DELETE, ch);
    return y + 48;
}

float tv_bar(lv_obj_t *root, float y)
{
    const auto &tv = G.d.tv;
    if (!tv || !tv->known || !in_view(tv->target) || !hub.can("tv.key")) {
        return y;
    }
    const float w = VIEW - 2 * PAD;
    lv_obj_t *bar = card(root, PAD, y, w, 50, 25);
    lv_obj_t *name = plain(bar, 0, 0, 90, 50);
    text(name, tv->name, 16, 9, 70, c.text, 12, semi());
    text(name, tv->on ? (tv->input.empty() ? "On" : tv->input) : "Off", 16, 27, 70, c.text2, 10);
    click(name, [] { open(Page::Music); });
    struct Key {
        const char *icon, *key;
    };
    static const Key keys[] = {{"power", ""}, {"play", "play"}, {"pause", "pause"}, {"voldown", "volume_down"}, {"volup", "volume_up"}};
    const float size = 36, gap = 6;
    float x = w - (5 * size + 4 * gap) - 8;
    const std::string target = tv->target;
    const bool on = tv->on;
    for (const Key &k : keys) {
        if (!strcmp(k.icon, "power")) {
            RoundOpts r;
            r.active = on;
            round_button(bar, x, 7, size, "power", [target, on] {
                act("tv.power", target, param("on", !on), on ? "Turn off the TV" : "Turn on the TV", "TV");
            }, r);
        } else {
            RoundOpts r;
            r.disabled = !on;
            const std::string key = k.key;
            round_button(bar, x, 7, size, k.icon, [target, key] { act("tv.key", target, param("key", key), "TV", "TV"); }, r);
        }
        x += size + gap;
    }
    return y + 58;
}

void cap_button(lv_obj_t *root, float x, float y, float w, const char *icon_name, const std::string &label,
                const std::string &word, std::function<void()> fn, bool alert)
{
    lv_obj_t *b = card(root, x, y, w, 44, 22);
    icon(b, alert ? "warn" : icon_name, 14, 13, 18, alert ? c.red : c.text, 1.3f);
    text(b, label, 40, 14, w * 0.45f, c.text, 12, semi());
    text(b, word, w * 0.45f, 15, w * 0.55f - 14, alert ? c.red : c.text2, 10, TextOpts{false, false, false, LV_TEXT_ALIGN_RIGHT});
    click(pressable(b), std::move(fn));
}

float plug_row(lv_obj_t *root, float y, const Item &item, const char *action, const std::string &words)
{
    const float w = VIEW - 2 * PAD;
    lv_obj_t *cardo = card(root, PAD, y, w, 58);
    text(cardo, item.name, 14, 10, w - 80, c.text, 12, semi());
    dot(cardo, 14, 37, 6, item.on ? c.ok : c.dot_off);
    text(cardo, words, 26, 33, w - 90, c.text2, 10);
    const std::string target = item.target, name = item.name, act_name = action;
    const bool on = item.on;
    std::string lower = name;
    for (auto &ch : lower) {
        ch = char(tolower((unsigned char)ch));
    }
    toggle(cardo, w - 58, 16, on, [target, name, lower, on, act_name] {
        lean(target, "on", !on);
        act(act_name, target, param("on", !on), name, lower);
    }, !hub.can(action) || !item.known);
    return y + 66;
}

void home_page()
{
    struct tm local {};
    const bool timed = displayClockTm(time(nullptr), uint8_t(prefs_clock_zone()), local);
    auto [title, hint] = status_sentence(G.d, timed ? &local : nullptr, G.noresp);
    lv_obj_t *sc = scroller(G.root, 0);
    float y = 12;
    text(sc, title, PAD + 2, y, 250, c.text, fit(title, 250, 18, true, 13), semi());
    BoxOpts more_o;
    more_o.radius = 18;
    more_o.field = true;
    lv_obj_t *more = pressable(box(sc, VIEW - 46, 8, 36, 36, more_o));
    text(more, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2", 0, 9, 36, c.text2, 12, semi(LV_TEXT_ALIGN_CENTER));
    click(more, [] { open(Page::Settings); });
    text(sc, hint, PAD + 2, y + 26, 250, c.text2, 10);
    y += 48;
    // Rooms with something this display can see (web home.js), as tall pills
    // in a row that keeps its place across refreshes.
    std::vector<std::pair<std::string, std::string>> shown_rooms;
    for (JsonObjectConst r : rooms())
        if (room_has_devices(r)) shown_rooms.push_back({r["id"] | "", r["name"] | ""});
    if (!shown_rooms.empty()) {
        lv_obj_t *row = plain(sc, PAD, y, VIEW - 2 * PAD, 38, 2);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        float x = 0, picked_x = 0, picked_w = 0;
        auto pick = [&](const std::string & id, const std::string & label) {
            const bool on = id == hub.prefs.room;
            const float wpx = text_w(label, 12, true) + 30;
            PillOpts p;
            p.on = on;
            p.size = 12;
            pill(row, x, 1, wpx, 34, label, [id] {
                if (hub.prefs.room == id) return;
                hub.prefs.room = id;
                G.last_scene.clear();
                hub.save();
                dirty();
            }, p);
            if (on) {
                picked_x = x, picked_w = wpx;
            }
            x += wpx + 6;
        };
        pick("", "All");
        for (const auto &r : shown_rooms) {
            pick(r.first, r.second);
        }
        lv_obj_update_layout(row);
        int32_t to = G.room_row_x;
        if (G.room_row_for != hub.prefs.room) {  // a new pick: bring it into view
            G.room_row_for = hub.prefs.room;
            const float view = VIEW - 2 * PAD;
            if (P(picked_x) < to || P(picked_x + picked_w) > to + P(view)) {
                to = int32_t(P(picked_x + picked_w / 2 - view / 2));
            }
        }
        lv_obj_scroll_to_x(row, std::max<int32_t>(0, to), LV_ANIM_OFF);
        lv_obj_add_event_cb(row, [](lv_event_t *e) {
            G.room_row_x = lv_obj_get_scroll_x(lv_event_get_target_obj(e));
        },
        LV_EVENT_SCROLL_END, nullptr);
        G.room_row_x = lv_obj_get_scroll_x(row);
        y += 44;
    }
    const bool room_lights = !room_bulbs(picked_room()).empty();
    if (room_lights && (hub.can("scene.apply") || hub.can("room.brightness"))) {
        y = scenes_bar(sc, y);
    }
    const bool show_lights = !room_bulbs(picked_room()).empty();
    const bool show_music = (G.d.speaker && in_view(G.d.speaker->target)) || (G.d.tv && in_view(G.d.tv->target));
    const float full = VIEW - 2 * PAD;
    if (show_lights && show_music) {
        tile(sc, PAD, y, full * 0.6f - 4, 116, true);
        tile(sc, PAD + full * 0.6f + 4, y, full * 0.4f - 4, 116, false);
        y += 124;
    } else if (show_lights || show_music) {
        tile(sc, PAD, y, full, 116, show_lights);
        y += 124;
    }
    y = tv_bar(sc, y);
    for (const Item &p : G.d.plugs)
        if (in_view(p.target)) {
            y = plug_row(sc, y, p, "plug.power", p.known ? (p.on ? "On" : "Off") : (p.reason.empty() ? "Not reporting" : p.reason));
        }
    for (const Item &a : G.d.acs) {
        if (!in_view(a.target)) {
            continue;
        }
        std::string words = a.known ? (a.on ? (a.mode.empty() ? "On" : a.mode) + (a.temp.empty() ? "" : " to " + a.temp + "\xC2\xB0") : "Off") : "Not reporting";
        if (a.known && !a.indoor.empty()) {
            words += " \xC2\xB7 " + a.indoor + "\xC2\xB0 inside";
        }
        y = plug_row(sc, y, a, "ac.power", words);
    }
    struct Small {
        const char *icon;
        std::string label, word;
        std::function<void()> fn;
        bool alert;
    };
    std::vector<Small> smalls;
    if ((G.d.feeder && in_view(G.d.feeder->target)) || (G.d.litter && in_view(G.d.litter->target))) {
        std::string word = "litter box";
        if (!G.pending_pets.empty()) {
            word = G.pending_pets;
        } else if (G.d.feeder) {
            word = G.d.feeder->known ? (G.d.feeder->last.empty() ? "no feed yet" : "fed " + G.d.feeder->last) : "not reporting";
        }
        smalls.push_back({"paw", "Pets", word, [] { open(Page::Pets); }, false});
    }
    if (G.d.camera && in_view(G.d.camera->target)) {
        const bool ok = G.d.camera->known && !(G.d.camera->has_streaming && !G.d.camera->streaming);
        smalls.push_back({"camera", "Camera", G.d.camera->known ? (ok ? "answering" : "no answer") : "not reporting",
                          [] { toast("The camera has no picture here.", "ok"); }, G.d.camera->known && !ok});
    }
    if (G.d.printer && in_view(G.d.printer->target)) {
        const Item &p = *G.d.printer;
        std::string word = !p.known ? "not reporting" : p.state == "printing" ? std::to_string(int(floor(p.pct + 0.5))) + "% done" : p.state;
        smalls.push_back({"printer", "Printer", word, [] {}, false});
    }
    for (const Item &o : G.d.others)
        if (in_view(o.target)) smalls.push_back({"tech", o.name, o.known ? "" : "not reporting", [] {}, false});
    const float half = (full - 8) / 2;
    for (size_t i = 0; i < smalls.size(); ++i) {
        cap_button(sc, PAD + (i % 2) * (half + 8), y + (i / 2) * 52, half, smalls[i].icon, smalls[i].label, smalls[i].word, smalls[i].fn, smalls[i].alert);
    }
    y += ((smalls.size() + 1) / 2) * 52;
    if (!hub.status.live) {
        y += 4;
        text(sc, "Dry-run. Nothing on this page is sent to any device.", PAD + 2, y, full, c.amber, 10, wrapped());
        y += 20;
    }
    if (!hub.status.connected && !hub.status.message.empty()) {
        text(sc, hub.status.message, PAD + 2, y + 4, full, c.amber, 10, wrapped());
    }
}

// ---- sub-pages ---------------------------------------------------------------------------
struct Hue {
    const char *name;
    int hue;
};
const Hue HUES[] = {{"Warm", 34}, {"Cool", 210}, {"Red", 0}, {"Orange", 28}, {"Yellow", 52}, {"Green", 120}, {"Blue", 225}, {"Purple", 270}, {"Pink", 320}};

void lights_page()
{
    JsonObjectConst room = picked_room();
    heading(G.root, room.isNull() ? "Lights" : std::string(room["name"] | "") + " lights", [] { open(Page::Home); });
    lv_obj_t *sc = scroller(G.root, 40);
    float y = 4;
    const auto bulbs = room_bulbs(room);
    for (const Item &b : bulbs) {
        const float w = VIEW - 2 * PAD;
        lv_obj_t *cardo = card(sc, PAD, y, w, b.color ? 128 : 96);
        text(cardo, b.name, 14, 10, w - 80, c.text, 12, semi());
        text(cardo, b.known ? (b.on ? std::to_string(b.bri) + "%" : "Off") : "Not reporting", 14, 28, w - 80, c.text2, 10);
        std::string lower = b.name;
        for (auto &ch : lower) {
            ch = char(tolower((unsigned char)ch));
        }
        const std::string target = b.target, name = b.name;
        const bool on = b.on;
        toggle(cardo, w - 58, 12, on, [ = ] { lean(target, "on", !on); act("light.power", target, param("on", !on), name, lower); },
               !b.known || !hub.can("light.power"));
        const float bw = w - 28;
        BoxOpts track_o;
        track_o.bg = c.field;
        track_o.bw = 0;
        track_o.radius = 13;
        track_o.field = true;
        lv_obj_t *track = box(cardo, 14, 54, bw, 26, track_o);
        const int v = G.drag.count(target) ? G.drag[target] : (b.on ? b.bri : 0);
        BoxOpts fill_o;
        fill_o.bg = hsl(b.hue, 85, c.lit_l);
        fill_o.bw = 0;
        fill_o.radius = 13;
        fill_o.hidden = v < 1;
        lv_obj_t *fill = box(track, 0, 0, std::max(0.01f, bw * v / 100), 26, fill_o);
        glow_fill(fill, hsl(b.hue, 85, c.lit_l));
        TextOpts pct_o = semi();
        pct_o.hidden = v < 12;
        lv_obj_t *pct = text(track, std::to_string(v) + "%", 10, 6, 60, 0x15121D, 10, pct_o);
        drag_slider(track, 0, 0, bw, 26, v, [ = ](int nv, bool released) {
            G.drag[target] = nv;
            lv_obj_set_width(fill, std::max(1, P(bw * nv / 100)));
            if (nv >= 1) {
                lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
            }
            lv_label_set_text(pct, (std::to_string(nv) + "%").c_str());
            if (nv >= 12) {
                lv_obj_remove_flag(pct, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(pct, LV_OBJ_FLAG_HIDDEN);
            }
            if (!released) {
                return;
            }
            G.drag.erase(target);
            dirty();
            lean(target, "on", nv > 0);
            if (nv > 0) {
                lean(target, "bri", nv);
                act("light.brightness", target, param("percent", std::max(10, nv)), name, lower);
            } else {
                act("light.power", target, param("on", false), name, lower);
            }
        });
        if (b.color && hub.can("light.hue")) {
            const float cw = (bw - 8 * 4) / 9;
            for (int i = 0; i < 9; ++i) {
                const Hue h = HUES[i];
                const uint32_t col = !strcmp(h.name, "Warm") ? hsl(34, 80, 70) : !strcmp(h.name, "Cool") ? hsl(210, 60, 80) : hsl(h.hue, 80, 60);
                BoxOpts chip_o;
                chip_o.bg = col;
                chip_o.bw = 0;
                chip_o.radius = 13;
                lv_obj_t *chip = pressable(box(cardo, 14 + i * (cw + 4), 90, cw, 26, chip_o));
                if (b.on && abs(b.hue - h.hue) < 3) {
                    lv_obj_set_style_outline_width(chip, 2, 0);
                    lv_obj_set_style_outline_pad(chip, 2, 0);
                    lv_obj_set_style_outline_color(chip, lv_color_hex(c.text), 0);
                }
                const int hue = h.hue;
                click(chip, [ = ] { lean(target, "hue", hue); act("light.hue", target, param("hue", hue), name, lower); });
            }
        }
        y += b.color ? 136 : 104;
    }
    if (bulbs.empty()) {
        text(sc, "No lights here yet.", PAD, y, 280, c.text2, 11);
    }
}

void music_page()
{
    heading(G.root, "Music & TV", [] { open(Page::Home); });
    lv_obj_t *sc = scroller(G.root, 40);
    float y = 4;
    const float w = VIEW - 2 * PAD;
    if (G.d.speaker) {
        const Item sp = *G.d.speaker;
        lv_obj_t *cardo = card(sc, PAD, y, w, 112);
        text(cardo, sp.name, 14, 10, w - 28, c.text, 12, semi());
        text(cardo, sp.known ? (sp.playing ? std::string("Playing") : (sp.source.empty() ? "Idle" : sp.source)) + (sp.mute ? " \xC2\xB7 muted" : "") : "Not reporting",
             14, 28, w - 28, c.text2, 10);
        const float bw = w - 28;
        BoxOpts track_o;
        track_o.bg = c.field;
        track_o.bw = 0;
        track_o.radius = 13;
        track_o.field = true;
        lv_obj_t *track = box(cardo, 14, 48, bw, 26, track_o);
        const int v = G.drag.count("music") ? G.drag["music"] : sp.vol;
        BoxOpts fill_o;
        fill_o.bg = c.fill_a;
        fill_o.bw = 0;
        fill_o.radius = 13;
        fill_o.hidden = v < 1;
        lv_obj_t *fill = box(track, 0, 0, std::max(0.01f, bw * v / 100), 26, fill_o);
        glow_fill(fill, c.fill_a);
        TextOpts vol_o = semi();
        vol_o.hidden = v < 30;
        lv_obj_t *label = text(track, "Volume " + std::to_string(v), 10, 6, 100, c.on_tint, 10, vol_o);
        const std::string target = sp.target;
        drag_slider(track, 0, 0, bw, 26, v, [ = ](int nv, bool released) {
            G.drag["music"] = nv;
            lv_obj_set_width(fill, std::max(1, P(bw * nv / 100)));
            if (nv >= 1) {
                lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
            }
            lv_label_set_text(label, ("Volume " + std::to_string(nv)).c_str());
            if (nv >= 30) {
                lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            }
            if (!released) {
                return;
            }
            G.drag.erase("music");
            dirty();
            lean(target, "vol", nv);
            act("speaker.volume", target, param("level", nv), "Volume", "speaker");
        });
        const float bwid = (bw - 18) / 4;
        struct B {
            std::string label;
            std::function<void()> fn;
            bool on;
        };
        const bool mute = sp.mute, night = sp.night;
        const B buttons[] = {
            {mute ? "Unmute" : "Mute", [ = ] { lean(target, "mute", !mute); act("speaker.mute", target, param("muted", !mute), "Mute", "speaker"); }, mute},
            {"Night", [ = ] { act("speaker.night", target, param("on", !night), "Night mode", "speaker"); }, night},
            {"Play", [ = ] { act("speaker.play", target, no_params(), "Play", "speaker"); }, sp.playing},
            {"Pause", [ = ] { act("speaker.pause", target, no_params(), "Pause", "speaker"); }, false},
        };
        for (int i = 0; i < 4; ++i) {
            PillOpts p;
            p.on = buttons[i].on;
            p.size = 10;
            pill(cardo, 14 + i * (bwid + 6), 80, bwid, 26, buttons[i].label, buttons[i].fn, p);
        }
        y += 120;
    }
    if (G.d.tv) {
        const Item tv = *G.d.tv;
        lv_obj_t *cardo = card(sc, PAD, y, w, 220);
        text(cardo, tv.name, 14, 10, w - 80, c.text, 12, semi());
        text(cardo, tv.known ? (tv.on ? (tv.input.empty() ? "On" : tv.input) : "Off") : "Not reporting", 14, 28, w - 80, c.text2, 10);
        const std::string target = tv.target;
        const bool on = tv.on;
        RoundOpts r;
        r.active = on;
        round_button(cardo, w - 50, 10, 36, "power", [ = ] { act("tv.power", target, param("on", !on), on ? "Turn off the TV" : "Turn on the TV", "TV"); }, r);
        auto key = [&](float x, float yy, const char *label, const char *k, const char *icon_name) {
            BoxOpts o;
            o.radius = 12;
            o.field = true;
            o.fill = !strcmp(k, "ok") && on;
            o.opa = on ? 255 : 110;
            lv_obj_t *b = pressable(box(cardo, x, yy, 46, 34, o));
            if (icon_name) {
                icon(b, icon_name, 14, 8, 18, c.text, 1.3f);
            } else {
                text(b, label, 0, 9, 46, c.text, 10, semi(LV_TEXT_ALIGN_CENTER));
            }
            const std::string ks = k, ls = label;
            if (on) click(b, [ = ] { act("tv.key", target, param("key", ks), ls, "TV"); });
        };
        const float cx = 22, cy = 56;
        key(cx + 50, cy, "Up", "up", "up");
        key(cx, cy + 40, "Left", "left", "back");
        key(cx + 50, cy + 40, "OK", "ok", nullptr);
        key(cx + 100, cy + 40, "Right", "right", "chev");
        key(cx + 50, cy + 80, "Down", "down", "down");
        const float rx = 180;
        key(rx, cy, "Vol +", "volume_up", nullptr);
        key(rx + 52, cy, "Ch +", "channel_up", nullptr);
        key(rx, cy + 40, "Vol \xE2\x88\x92", "volume_down", nullptr);
        key(rx + 52, cy + 40, "Ch \xE2\x88\x92", "channel_down", nullptr);
        key(rx, cy + 80, "Back", "back", nullptr);
        key(rx + 52, cy + 80, "Home", "home", nullptr);
        JsonArrayConst inputs = capability(tv.dev, "inputs")["inputs"].as<JsonArrayConst>();
        const size_t n = inputs.size();
        const float iw = (w - 28 - (float(n) - 1) * 4) / float(std::max<size_t>(1, n));
        size_t i = 0;
        for (JsonVariantConst in : inputs) {
            const std::string name = in | "";
            PillOpts p;
            p.on = on && tv.input == name;
            p.size = 9;
            p.disabled = !on;
            pill(cardo, 14 + i * (iw + 4), 182, iw, 28, name, [ = ] { act("tv.input", target, param("input", name), name, "TV"); }, p);
            ++i;
        }
    }
}

void pets_page()
{
    heading(G.root, "Pets", [] { open(Page::Home); });
    lv_obj_t *sc = scroller(G.root, 40);
    float y = 4;
    const float w = VIEW - 2 * PAD;
    if (G.d.feeder) {
        const Item f = *G.d.feeder;
        lv_obj_t *cardo = card(sc, PAD, y, w, 120);
        text(cardo, f.name, 14, 10, w - 28, c.text, 12, semi());
        text(cardo, f.known ? (f.last.empty() ? "No feed yet" : "Last fed " + f.last) : "Not reporting", 14, 28, w - 28, c.text2, 10);
        std::vector<int> grams;
        for (JsonVariantConst g : capability(f.dev, "dispense")["grams"].as<JsonArrayConst>()) {
            grams.push_back(g | 0);
        }
        if (grams.empty()) grams = {10, 20, 30, 40, 50};
        const float gw = (w - 28 - (float(grams.size()) - 1) * 6) / float(grams.size());
        const std::string target = f.target;
        for (size_t i = 0; i < grams.size(); ++i) {
            const int g = grams[i];
            PillOpts p;
            p.disabled = !f.known || !hub.can("pets.feed");
            pill(cardo, 14 + i * (gw + 6), 56, gw, 34, std::to_string(g) + " g",
                 [ = ] { act("pets.feed", target, param("grams", g), "Feed " + std::to_string(g) + " g", "feeder"); }, p);
        }
        text(cardo, "Feeding asks you to confirm.", 14, 98, w - 28, c.text3, 9);
        y += 128;
    }
    if (G.d.litter) {
        const Item lb = *G.d.litter;
        lv_obj_t *cardo = card(sc, PAD, y, w, 92);
        text(cardo, lb.name, 14, 10, w - 28, c.text, 12, semi());
        text(cardo, lb.known ? (lb.busy ? "Cleaning\xE2\x80\xA6" : lb.last.empty() ? "Ready" : "Last cleaned " + lb.last) : "Not reporting", 14, 28, w - 28, c.text2, 10);
        PillOpts p;
        p.disabled = !lb.known || lb.busy || !hub.can("pets.clean");
        const std::string target = lb.target;
        pill(cardo, 14, 50, w - 28, 32, "Clean cycle", [ = ] { act("pets.clean", target, no_params(), "Clean cycle", "litter box"); }, p);
    }
}

// Tier 2: what will happen, its consequences, and Go / Cancel (web core/guards.js).
void confirm_page()
{
    if (!G.prep) {
        open(Page::Home);
        return;
    }
    const JsonDocument &prep = *G.prep;
    const float w = VIEW - 2 * PAD;
    lv_obj_t *cardo = card(G.root, PAD, 30, w, 240, 22);
    text(cardo, prep["label"] | "Confirm", 18, 18, w - 36, c.text, 16, semi());
    float y = 46;
    if (prep["target_name"].is<const char *>()) {
        text(cardo, prep["target_name"].as<const char *>(), 18, y, w - 36, c.text2, 10);
        y += 20;
    }
    if (prep["what"].is<const char *>()) {
        const std::string what = prep["what"].as<const char *>();
        text(cardo, what, 18, y, w - 36, c.text, 11, wrapped());
        y += text_h(what, w - 36, 11) + 8;
    }
    for (JsonVariantConst cons : prep["consequences"].as<JsonArrayConst>()) {
        const std::string line = "\xE2\x80\xA2 " + std::string(cons | "");
        text(cardo, line, 18, y, w - 36, c.text2, 10, wrapped());
        y += text_h(line, w - 36, 10) + 4;
    }
    if (prep["warning"].is<const char *>()) {
        text(cardo, prep["warning"].as<const char *>(), 18, y, w - 36, c.amber, 10, wrapped());
    }
    const float half = (w - 36 - 10) / 2;
    pill(cardo, 18, 186, half, 40, "Cancel", [] { lean_settle(G.prep_target, false); G.prep.reset(); open(Page::Home); });
    PillOpts go;
    go.on = true;
    pill(cardo, 28 + half, 186, half, 40, prep["go_label"] | "Go", [] {
        DocPtr prepared = G.prep;
        const std::string name = G.prep_name.empty() ? "device" : G.prep_name;
        G.prep.reset();
        open(Page::Home);
        hub.confirm(prepared, after_run(name, G.prep_target));
    }, go);
}

bool apply_theme();

void settings_page()
{
    heading(G.root, "Settings", [] { open(Page::Home); });
    lv_obj_t *sc = scroller(G.root, 40);
    float y = 4;
    const float w = VIEW - 2 * PAD;
    lv_obj_t *appearance = card(sc, PAD, y, w, 56);
    text(appearance, "Appearance", 14, 18, 90, c.text, 12, semi());
    static const char *const options[] = {"Light", "AMOLED", "Auto"};
    for (int i = 0; i < 3; ++i) {
        PillOpts p;
        p.on = hub.prefs.theme == i;
        p.size = 11;
        pill(appearance, 110 + i * 62, 12, 58, 32, options[i], [i] { hub.prefs.theme = i; hub.save(); apply_theme(); dirty(); }, p);
    }
    y += 64;
    lv_obj_t *corners = card(sc, PAD, y, w, 56);
    text(corners, "Screen corners", 14, 10, w - 28, c.text, 12, semi());
    text(corners, "Launcher (AI key) \xE2\x86\x92 Settings \xE2\x86\x92 Corners.", 14, 30, w - 28, c.text2, 10);
    y += 64;
    lv_obj_t *info = card(sc, PAD, y, w, 76);
    text(info, "Paired with", 14, 10, w - 28, c.text2, 10);
    text(info, hub.cfg.hub, 14, 26, w - 28, c.text, 12, semi());
    text(info, "As " + (hub.cfg.name.empty() ? std::string("Wall display") : hub.cfg.name) + " \xC2\xB7 " +
         (hub.status.connected ? std::string("connected") : hub.status.message), 14, 48, w - 28, c.text2, 10);
    y += 84;
    text(sc, "The owner can remove this display from the web app: More \xE2\x86\x92 Passkeys \xE2\x86\x92 Wall displays.", PAD + 2, y, w, c.text3, 9, wrapped());
    y += 30;
    PillOpts red;
    red.color = c.red;
    pill(sc, PAD, y, w, 40, G.confirm_unpair ? "Tap again to unpair" : "Unpair this display", [] {
        if (G.confirm_unpair && !refused_read_only()) {
            G.confirm_unpair = false;
            hub.unpair();
            open(Page::Pair);
        } else {
            G.confirm_unpair = true;
            dirty();
        }
    }, red);
    y += 48;
    pill(sc, PAD, y, w, 40, "Exit to Mosaico launcher", [] { if (G.exit_to_launcher) G.exit_to_launcher(); });
}

// ---- pairing and the draft keyboard ---------------------------------------------------
void edit_field(const std::string &title, const std::string &value, bool upper, size_t limit,
                std::function<void(const std::string &)> done)
{
    G.edit.reset(new EditState{title, value, upper ? "upper" : "lower", upper, limit,
                               [done](const std::string & t)
    {
        done(t);
        G.edit.reset();
        open(Page::Pair);
    }});
    open(Page::Edit);
}

void pair_page()
{
    const float w = VIEW - 2 * PAD;
    text(G.root, "Swifty Home", PAD + 2, 14, w, c.text, 20, semi());
    text(G.root, "Pair this display with your hub.", PAD + 2, 42, w, c.text2, 11);
    auto field = [&](float y, const char *label, const std::string & value, std::function<void()> fn) {
        lv_obj_t *b = card(G.root, PAD, y, w, 52, 14);
        text(b, label, 14, 8, w - 28, c.text2, 10);
        text(b, value, 14, 26, w - 40, c.text, 13, semi());
        icon(b, "chev", w - 28, 18, 16, c.tint_soft, 1.6f);
        click(pressable(b), std::move(fn));
    };
    field(66, "Hub address", G.pair.hub.empty() ? "https://\xE2\x80\xA6" : G.pair.hub,
          [] { edit_field("Hub address", G.pair.hub, false, 200, [](const std::string & t)
    {
        G.pair.hub = t;
    });
             });
    field(126, "Pairing code", G.pair.code.empty() ? "From the owner's More \xE2\x86\x92 Passkeys \xE2\x86\x92 Wall displays" : G.pair.code,
          [] { edit_field("Pairing code", G.pair.code, true, 12, [](const std::string & t)
    {
        G.pair.code = t;
    });
             });
    if (!G.pair.message.empty()) {
        text(G.root, G.pair.message, PAD + 2, 188, w, G.pair.ok ? c.ok : c.red, 10, wrapped());
    }
    PillOpts go;
    go.on = true;
    pill(G.root, PAD, 236, w, 44, G.pair.busy ? "Pairing\xE2\x80\xA6" : "Pair", [] {
        if (G.pair.busy) return;
        const std::string &h = G.pair.hub;
        const bool valid_hub = (h.rfind("https://", 0) == 0 && h.size() > 8) || (h.rfind("http://", 0) == 0 && h.size() > 7);
        if (!valid_hub || h.find(' ') != std::string::npos) {
            G.pair.message = "Enter the hub address, like https://home.example.org";
            dirty();
            return;
        }
        std::string code;
        for (char ch : G.pair.code) if (ch != ' ' && ch != '-') code += ch;
        if (code.size() != 8)
        {
            G.pair.message = "The code has 8 letters and numbers.";
            dirty();
            return;
        }
        G.pair.busy = true;
        G.pair.message.clear();
        hub.pair(G.pair.hub, G.pair.code, [](bool ok, const std::string & message)
        {
            G.pair.busy = false;
            if (ok) {
                G.pair.code.clear();
                G.pair.message.clear();
                open(Page::Home);
            } else {
                G.pair.message = message;
                G.pair.ok = false;
            }
            dirty();
        });
        dirty();
    }, go);
}

// Typing: a text field and the shared Mosaico keyboard, built once (the page
// is not rebuilt while it is open: see tick()).
void edit_page()
{
    if (!G.edit) {
        open(Page::Pair);
        return;
    }
    heading(G.root, G.edit->title, [] { G.edit.reset(); open(Page::Pair); });
    lv_obj_t *ta = lv_textarea_create(G.root);
    lv_obj_set_pos(ta, P(PAD), P(42));
    lv_obj_set_size(ta, P(VIEW - 2 * PAD), P(38));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, uint32_t(G.edit->limit));
    lv_textarea_set_text(ta, G.edit->text.c_str());
    lv_obj_set_style_bg_color(ta, lv_color_hex(c.card), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(c.tint), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, P(10), 0);
    lv_obj_set_style_pad_hor(ta, P(10), 0);
    lv_obj_set_style_pad_ver(ta, P(9), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(c.text), 0);
    lv_obj_set_style_text_font(ta, font(15, false), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(c.tint), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(ta, 2, LV_PART_CURSOR);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ta, [](lv_event_t *e) {
        if (G.edit) {
            G.edit->text = lv_textarea_get_text(lv_event_get_current_target_obj(e));
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(ta, [](lv_event_t *e) {
        if (!G.edit) {
            return;
        }
        const auto done = G.edit->done;
        const std::string text = lv_textarea_get_text(lv_event_get_current_target_obj(e));
        done(text);  // opens another page; the rebuild happens on the next tick
    }, LV_EVENT_READY, nullptr);
    mui::KeyboardConfig kc;
    kc.action = "Done";
    kc.accent = &mui::VIOLET;
    kc.caps_only = G.edit->upper_only;
    kc.auto_caps = false;  // hub addresses and codes
    const int w = lv_obj_get_width(G.root), h = lv_obj_get_height(G.root);
    mui::keyboard(G.root, ta, 0, h - 236, w, 230, kc);
}

void draw_toast()
{
    if (!G.toast.shown) {
        return;
    }
    if (now_ms() - G.toast.at > 4000) {
        G.toast.shown = false;
        return;
    }
    const float w = VIEW - 40;
    const bool fail = G.toast.kind == "fail";
    BoxOpts o;
    o.radius = 20;
    o.bg = fail ? c.red : c.text;
    o.bw = 0;
    o.fill = !fail && !light;
    lv_obj_t *b = box(G.root, 20, VIEW_H - 64, w, 40, o);
    text(b, G.toast.text, 14, 12, w - 28, o.fill ? c.on_tint : c.bg, 11, semi(LV_TEXT_ALIGN_CENTER));
    click(b, [] { G.toast.shown = false; dirty(); });
}

// Light, Dark, or Auto: dark 22:00-06:00 like the web Wall screen.
bool apply_theme()
{
    bool want = hub.prefs.theme == 0;
    if (hub.prefs.theme == 2) {
        struct tm local {};
        want = !displayClockTm(time(nullptr), uint8_t(prefs_clock_zone()), local) || (local.tm_hour >= 6 && local.tm_hour < 22);
    }
    if (want == light) {
        return false;
    }
    light = want;
    c = light ? light_palette() : dark_palette();
    return true;
}

bool finger_down()
{
    for (lv_indev_t *i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i))
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER &&
                (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED || lv_indev_get_scroll_obj(i))) {
            return true;
        }
    return G.scroller && lv_obj_is_scrolling(G.scroller);
}

void render()
{
    G.dirty = false;
    if (apply_theme()) {
        G.has_rendered = false;
    }
    if (!hub.paired() && G.page != Page::Edit) {
        G.page = Page::Pair;
    }
    const bool same_page = G.has_rendered && G.rendered == G.page;
    const int32_t scroll = same_page && G.scroller ? lv_obj_get_scroll_y(G.scroller) : 0;
    lv_obj_clean(G.root);
    G.scroller = nullptr;
    // AMOLED: the frame is clear over the screen's horizon backdrop.
    lv_obj_set_style_bg_color(G.root, lv_color_hex(c.bg), 0);
    lv_obj_set_style_bg_opa(G.root, light ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(lv_obj_get_screen(G.root), lv_color_hex(c.bg), 0);
    if (G.backdrop) {
        if (light) {
            lv_obj_add_flag(G.backdrop, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(G.backdrop, LV_OBJ_FLAG_HIDDEN);
        }
    }
    G.d = build();
    switch (G.page) {
    case Page::Pair: pair_page(); break;
    case Page::Edit: edit_page(); break;
    case Page::Lights: lights_page(); break;
    case Page::Music: music_page(); break;
    case Page::Pets: pets_page(); break;
    case Page::Confirm: confirm_page(); break;
    case Page::Settings: settings_page(); break;
    default: home_page(); break;
    }
    draw_toast();
    if (G.scroller && scroll) {
        lv_obj_update_layout(G.scroller);
        lv_obj_scroll_to_y(G.scroller, scroll, LV_ANIM_OFF);
    }
    // A new page rises in card by card; data refreshes stay still.
    if (!same_page) {
        mui::stagger(G.scroller ? G.scroller : G.root, 0, 15);
    }
    G.rendered = G.page;
    G.has_rendered = true;
    G.last_render = now_ms();
}

void tick(lv_timer_t *)
{
    const bool changed = hub.poll();
    hub.tick_timers();
    if (!G.active) {
        return;
    }
    if (hub.status.revision != G.last_revision) {
        G.last_revision = hub.status.revision;
        G.dirty = true;
    }
    if (changed) {
        G.dirty = true;
    }
    if (G.page == Page::Home && leans_due()) {
        G.dirty = true;
    }
    // Toasts time out; the home page's times are minutes, so a refresh every
    // 30 s is enough (it rebuilt every second before, and stuttered).
    if (!G.dirty && ((G.toast.shown && now_ms() - G.last_render > 1000) ||
                     (G.page == Page::Home && now_ms() - G.last_render > 30000))) {
        G.dirty = true;
    }
    // The keyboard page keeps its shift state and text: only leaving rebuilds.
    if (G.dirty && G.page == Page::Edit && G.rendered == Page::Edit && G.has_rendered) {
        G.dirty = false;
    }
    // Never rebuild under a finger: the slider or scroll keeps its own state.
    if (G.dirty && !finger_down()) {
        render();
    }
}
}  // namespace

// Host preview hooks.
void home_preview_open(int page)
{
    G.page = Page(page);
    render();
}
void home_preview_theme(int mode)
{
    hub.prefs.theme = mode;
    render();
}
void home_preview_toast(const char *message)
{
    toast(message, "ok");
    render();
}

}  // namespace home

extern "C" void home_app_set_read_only(bool on)
{
    home::read_only = on;
}

extern "C" void home_app_set_exit_handler(void (*exit_to_launcher)())
{
    home::G.exit_to_launcher = exit_to_launcher;
}

extern "C" void home_app_start(lv_obj_t *frame)
{
    using namespace home;
    if (G.built) {
        return;
    }
    G.built = true;
    G.root = frame;
    lv_obj_update_layout(frame);  // the frame was just sized
    S = float(lv_obj_get_width(frame)) / VIEW;
    VIEW_H = float(lv_obj_get_height(frame)) / S;
    // Violet horizon and a warm bloom behind the lights, under the frame.
    lv_obj_t *screen = lv_obj_get_screen(frame);
    G.backdrop = mui::backdrop(screen, {&mui::VIOLET, true, 505, 38, 60, 270});
    lv_obj_move_to_index(G.backdrop, 0);
    hub.start();
    hub.set_active(false);
    G.page = hub.paired() ? Page::Home : Page::Pair;
    lv_timer_create(tick, 50, nullptr);
}

extern "C" void home_app_summary(home_summary_t *out)
{
    using namespace home;
    memset(out, 0, sizeof(*out));
    out->started = G.built;
    out->paired = hub.paired();
    out->connected = hub.status.connected;
    if (!G.built) {
        strlcpy(out->title, "Swifty Home", sizeof(out->title));
        return;
    }
    if (!out->paired) {
        strlcpy(out->title, "Not paired yet", sizeof(out->title));
        strlcpy(out->hint, "Open Home to pair this display.", sizeof(out->hint));
        return;
    }
    const Derived d = build();
    struct tm local {};
    const bool timed = displayClockTm(time(nullptr), uint8_t(prefs_clock_zone()), local);
    auto [title, hint] = status_sentence(d, timed ? &local : nullptr, G.noresp);
    strlcpy(out->title, title.c_str(), sizeof(out->title));
    strlcpy(out->hint, hint.c_str(), sizeof(out->hint));
    if (!d.bulbs.empty()) {
        const LightsSummary l = lights_summary(d.bulbs);
        out->lights = true;
        out->lights_pct = l.n > 0 ? l.avg : 0;
        out->lights_on = l.n;
        out->lights_count = l.total;
        out->lights_hue = int(l.hue);
        strlcpy(out->lights_state, l.state.c_str(), sizeof(out->lights_state));
    }
    if (d.speaker) {
        out->music = true;
        out->volume = d.speaker->known ? d.speaker->vol : -1;
        strlcpy(out->music_state, !d.speaker->known ? "Not reporting" : d.speaker->playing ? "Playing"
                : d.speaker->mute ? "Muted" : "Idle", sizeof(out->music_state));
    }
    if (d.tv) {
        out->tv = true;
        out->tv_on = d.tv->known && d.tv->on;
        strlcpy(out->tv_name, d.tv->name.c_str(), sizeof(out->tv_name));
        const std::string state = !d.tv->known ? "Not reporting" : d.tv->on ? (d.tv->input.empty() ? "On" : "On \xC2\xB7 " + d.tv->input) : "Off";
        strlcpy(out->tv_state, state.c_str(), sizeof(out->tv_state));
    }
    for (const Item &p : d.plugs) {
        if (out->plug_count == 2) {
            break;
        }
        auto &plug = out->plugs[out->plug_count++];
        strlcpy(plug.name, p.name.c_str(), sizeof(plug.name));
        plug.on = p.on;
        plug.known = p.known;
    }
}

extern "C" void home_app_open(lv_obj_t *frame)
{
    using namespace home;
    home_app_start(frame);
    G.active = true;
    hub.set_active(true);
    hub.refresh();
    G.dirty = true;
}

extern "C" void home_app_set_active(bool active)
{
    home::G.active = active && home::G.built;
    home::hub.set_active(home::G.active);
}

namespace {
std::string pairing_note;  // the last pairing started from home_app_pair
}

extern "C" void home_app_pair(const char *hub_url, const char *code)
{
    using namespace home;
    const std::string h = hub_url ? hub_url : "";
    std::string clean;
    for (const char *c = code ? code : ""; *c; ++c)
        if (*c != ' ' && *c != '-') {
            clean += *c;
        }
    if (!G.built) {
        pairing_note = "failed: Home has not started yet";
        return;
    }
    const bool valid_hub = (h.rfind("https://", 0) == 0 && h.size() > 8) || (h.rfind("http://", 0) == 0 && h.size() > 7);
    if (!valid_hub || h.find(' ') != std::string::npos) {
        pairing_note = "failed: home.hub must be the hub address, like https://home.example.org";
        return;
    }
    if (clean.size() != 8) {
        pairing_note = "failed: home.pairing_code has 8 letters and numbers";
        return;
    }
    if (G.pair.busy) {
        pairing_note = "failed: a pairing is already running";
        return;
    }
    G.pair.busy = true;
    pairing_note = "pairing";
    hub.pair(h, clean, [](bool ok, const std::string & message) {
        G.pair.busy = false;
        pairing_note = ok ? "paired as " + hub.cfg.name : "failed: " + message;
        if (ok) {
            G.pair.code.clear();
            G.pair.message.clear();
            open(Page::Home);
        }
        dirty();
    });
}

extern "C" void home_app_pairing_info(home_pairing_info_t *out)
{
    using namespace home;
    memset(out, 0, sizeof(*out));
    out->paired = hub.paired();
    strlcpy(out->hub, hub.cfg.hub.c_str(), sizeof(out->hub));
    strlcpy(out->name, hub.cfg.name.c_str(), sizeof(out->name));
    strlcpy(out->note, pairing_note.c_str(), sizeof(out->note));
}

// ---- Swifty's tools and shortcuts -------------------------------------------------------------
namespace home {
namespace {
std::string lower(std::string s)
{
    for (char &ch : s) {
        ch = char(tolower(uint8_t(ch)));
    }
    return s;
}

bool matches(const std::string &name, const char *want)
{
    if (!want || !*want || name.empty()) {
        return false;
    }
    const std::string a = lower(name), b = lower(want);
    return a == b || a.find(b) != std::string::npos || b.find(a) != std::string::npos;
}

bool same_name(const std::string &name, const std::string &want)
{
    return !name.empty() && lower(name) == lower(want);
}

void put(char *out, size_t cap, const std::string &text)
{
    strlcpy(out, text.c_str(), cap);
}

// One hub action a command turns into.
struct Planned {
    std::string action, target, label, name;
    JsonDocument params;
};

struct Plan {
    std::vector<Planned> acts;
    std::string what;      // "Living room lights off", or why it cannot be done
    std::string resolved;  // the target as the hub knows it: a room id, a device target, "all" or a scene id
};

template <typename T> Planned planned(const char *action, const std::string &target, const char *key, T value,
                                      const std::string &label, const char *name)
{
    Planned p{action, target, label, name, JsonDocument()};
    p.params[key] = value;
    return p;
}
Planned planned(const char *action, const std::string &target, const std::string &label, const char *name)
{
    Planned p{action, target, label, name, JsonDocument()};
    p.params.to<JsonObject>();
    return p;
}

const Item *find_item(const std::vector<Item> &list, const std::string &want, bool exact)
{
    for (const Item &i : list)
        if (exact ? (i.target == want || same_name(i.name, want)) : matches(i.name, want.c_str())) {
            return &i;
        }
    return nullptr;
}

JsonObjectConst find_room(const std::string &want, bool exact)
{
    for (JsonObjectConst r : rooms())
        if (exact ? (want == (r["id"] | "") || same_name(r["name"] | "", want)) : matches(r["name"] | "", want.c_str())) {
            return r;
        }
    return JsonObjectConst();
}

// What a command (verb, target, value; see home_app.h) sends. False: why not, in plan.what.
bool plan_command(const std::string &verb, const std::string &target, const std::string &value, Plan &plan)
{
    const Derived d = build();
    const bool all = target.empty() || same_name(target, "all") || same_name(target, "everything") ||
                     same_name(target, "all lights") || target == "all_bulbs";
    // Exact names (or ids) first, a room before a light; then near names.
    JsonObjectConst room;
    const Item *bulb = nullptr;
    if (!all) {
        room = find_room(target, true);
        if (room.isNull()) {
            bulb = find_item(d.bulbs, target, true);
        }
        if (room.isNull() && !bulb) {
            room = find_room(target, false);
        }
        if (room.isNull() && !bulb) {
            bulb = find_item(d.bulbs, target, false);
        }
    }
    const int number = atoi(value.c_str());
    auto fail = [&](const std::string & why) {
        plan.what = why;
        return false;
    };
    const bool lights_verb = verb == "lights_on" || verb == "lights_off" || verb == "lights_toggle" || verb == "brightness";
    if (lights_verb) {
        bool on = verb != "lights_off";
        if (verb == "lights_toggle") {
            // Off when any of them is on, else on.
            bool any_on = false;
            for (const Item &b : d.bulbs) {
                const bool in = bulb ? &b == bulb : !room.isNull() ? room_of(b.target) == room : all;
                any_on |= in && b.known && b.on;
            }
            on = !any_on;
        }
        const int pct = verb == "brightness" ? std::clamp(number, 0, 100) : 100;
        const bool dark = !on || pct == 0;
        // A toggle is told as one ("on or off"): which way it goes depends on the moment.
        const std::string toggled = verb == "lights_toggle" ? " on or off" : "";
        if (bulb) {
            plan.resolved = bulb->target;
            if (dark) {
                plan.acts.push_back(planned("light.power", bulb->target, "on", false, bulb->name, "light"));
            } else if (verb == "brightness") {
                plan.acts.push_back(planned("light.brightness", bulb->target, "percent", std::max(10, pct), bulb->name, "light"));
            } else {
                plan.acts.push_back(planned("light.power", bulb->target, "on", true, bulb->name, "light"));
            }
            plan.what = bulb->name + (!toggled.empty() ? toggled : dark ? " off" : verb == "brightness" ? " to " + std::to_string(pct) + "%" : " on");
            return true;
        }
        if (!room.isNull()) {
            const std::string id = room["id"] | "", name = room["name"] | "";
            plan.resolved = id;
            if (dark) {
                plan.acts.push_back(planned("room.power", id, "on", false, name + " lights", "lights"));
            } else {
                plan.acts.push_back(planned("room.brightness", id, "percent", pct, name + " lights", "lights"));
            }
            plan.what = name + " lights" + (!toggled.empty() ? toggled : dark ? " off" : verb == "brightness" ? " " + std::to_string(pct) + "%" : " on");
            return true;
        }
        if (all) {
            plan.resolved = "all";
            if (dark) {
                plan.acts.push_back(planned("lights.power", "all_bulbs", "on", false, "lights", "lights"));
            } else {
                plan.acts.push_back(planned("lights.brightness", "all_bulbs", "percent", std::max(1, pct), "lights", "lights"));
            }
            plan.what = std::string("All lights") + (!toggled.empty() ? toggled : dark ? " off" : verb == "brightness" ? " " + std::to_string(pct) + "%" : " on");
            return true;
        }
        return fail("No room or light called \"" + target + "\".");
    }
    if (verb == "color") {
        static const struct {
            const char *name;
            int hue;
        } NAMES[] = {{"warm", 34}, {"cool", 210}, {"red", 0}, {"orange", 28},
            {"yellow", 52}, {"green", 120}, {"blue", 225}, {"purple", 270}, {"pink", 320}
        };
        int hue = -1;
        for (const auto &n : NAMES)
            if (matches(n.name, value.c_str())) {
                hue = n.hue;
            }
        if (hue < 0 && !value.empty() && isdigit(uint8_t(value[0]))) {
            hue = number % 360;
        }
        if (hue < 0) {
            return fail("Name a colour (warm, cool, red, orange, yellow, green, blue, purple, pink) or a hue.");
        }
        for (const Item &b : d.bulbs)
            if ((bulb && &b == bulb) || (!bulb && !room.isNull() && room_of(b.target) == room) || (!bulb && room.isNull() && all))
                if (b.color) {
                    plan.acts.push_back(planned("light.hue", b.target, "hue", hue, b.name, "light"));
                }
        if (plan.acts.empty()) {
            return fail("No colour light matches \"" + target + "\".");
        }
        plan.resolved = bulb ? bulb->target : !room.isNull() ? std::string(room["id"] | "") : std::string("all");
        plan.what = std::to_string(plan.acts.size()) + " light(s) to " + value;
        return true;
    }
    if (verb == "scene") {
        // The hub's scenes by id or name; Movie is a scene that asks first.
        const std::string want = value.empty() ? target : value;
        std::string id, name;
        JsonArrayConst scenes = hub.scenes ? (*hub.scenes)["scenes"].as<JsonArrayConst>() : JsonArrayConst();
        for (int exact = 1; exact >= 0 && id.empty(); --exact)
            for (JsonObjectConst s : scenes) {
                const std::string sid = s["id"] | "", sname = s["name"] | "";
                if (exact ? (want == sid || same_name(sname, want)) : matches(sname, want.c_str())) {
                    id = sid, name = sname.empty() ? sid : sname;
                    break;
                }
            }
        if (id.empty()) {
            // Without the list (or on an older hub): the three every hub has.
            if (matches("movie", want.c_str())) {
                id = "movie", name = "Movie";
            } else if (matches("evening", want.c_str())) {
                id = "evening", name = "Evening";
            } else if (matches("lights off", want.c_str()) || matches("all off", want.c_str()) || same_name(want, "off")) {
                id = "all_off", name = "Lights off";
            }
        }
        if (id.empty()) {
            std::string names;
            for (JsonObjectConst s : scenes) {
                names += (names.empty() ? "" : ", ") + std::string(s["name"] | (s["id"] | ""));
            }
            return fail("Scenes: " + (names.empty() ? std::string("Movie, Evening, Lights off") : names) + ".");
        }
        plan.resolved = id;
        plan.acts.push_back(id == "movie" ? planned("scene.movie", id, name, "lights") : planned("scene.apply", id, name, "lights"));
        plan.what = "the " + name + " scene";
        return true;
    }
    if (verb == "volume" || verb == "play" || verb == "pause" || verb == "mute" || verb == "unmute") {
        if (!d.speaker) {
            return fail("No speaker.");
        }
        const std::string t = d.speaker->target;
        plan.resolved = t;
        if (verb == "volume") {
            plan.acts.push_back(planned("speaker.volume", t, "level", std::clamp(number, 0, 100), "Volume", "speaker"));
        } else if (verb == "play") {
            plan.acts.push_back(planned("speaker.play", t, "Play", "speaker"));
        } else if (verb == "pause") {
            plan.acts.push_back(planned("speaker.pause", t, "Pause", "speaker"));
        } else {
            plan.acts.push_back(planned("speaker.mute", t, "muted", verb == "mute", "Mute", "speaker"));
        }
        plan.what = verb == "volume" ? "volume " + std::to_string(std::clamp(number, 0, 100)) : verb;
        return true;
    }
    if (verb == "tv_on" || verb == "tv_off" || verb == "tv_toggle") {
        if (!d.tv) {
            return fail("No TV.");
        }
        const bool on = verb == "tv_toggle" ? !(d.tv->known && d.tv->on) : verb == "tv_on";
        plan.resolved = d.tv->target;
        plan.acts.push_back(planned("tv.power", d.tv->target, "on", on, on ? "Turn on the TV" : "Turn off the TV", "TV"));
        plan.what = verb == "tv_toggle" ? "TV on or off" : on ? "TV on" : "TV off";
        return true;
    }
    if (verb == "plug_on" || verb == "plug_off" || verb == "plug_toggle") {
        const Item *p = find_item(d.plugs, target, true);
        if (!p) {
            p = find_item(d.plugs, target, false);
        }
        if (!p) {
            return fail("No plug called \"" + target + "\".");
        }
        const bool on = verb == "plug_toggle" ? !(p->known && p->on) : verb == "plug_on";
        plan.resolved = p->target;
        plan.acts.push_back(planned("plug.power", p->target, "on", on, p->name, "plug"));
        plan.what = p->name + (verb == "plug_toggle" ? " on or off" : on ? " on" : " off");
        return true;
    }
    return fail("Unknown action \"" + verb + "\".");
}

// A run ends in one of these phases (web core/actions.js).
bool failed_phase(const std::string &phase)
{
    return phase == "no_response" || phase == "failed" || phase == "refused";
}
}  // namespace
}  // namespace home

extern "C" void home_app_describe(char *out, size_t capacity)
{
    using namespace home;
    if (!hub.paired()) {
        return put(out, capacity, "Not paired with Swifty Home.");
    }
    if (!hub.state) {
        return put(out, capacity, "No data from Swifty Home yet.");
    }
    const Derived d = build();
    std::string t = hub.status.connected ? "Swifty Home, live.\n" : "Swifty Home (unreachable; last known).\n";
    for (JsonObjectConst r : rooms()) {
        std::vector<std::string> lights;
        for (const Item &b : d.bulbs)
            if (room_of(b.target) == r) {
                lights.push_back(b.name + (b.known ? (b.on ? " on " + std::to_string(b.bri) + "%" : " off") : " not reporting"));
            }
        if (lights.empty()) {
            continue;
        }
        t += std::string("Room ") + (r["name"] | "") + ": ";
        for (size_t i = 0; i < lights.size(); ++i) {
            t += (i ? ", " : "") + lights[i];
        }
        t += "\n";
    }
    if (d.speaker) t += "Speaker " + d.speaker->name + ": " + (d.speaker->playing ? "playing" : "idle") + ", volume " +
                            std::to_string(d.speaker->vol) + (d.speaker->mute ? ", muted" : "") + "\n";
    if (d.tv) {
        t += "TV " + d.tv->name + ": " + (d.tv->on ? "on" : "off") + (d.tv->input.empty() ? "" : ", input " + d.tv->input) + "\n";
    }
    for (const Item &p : d.plugs) {
        t += "Plug " + p.name + ": " + (p.known ? (p.on ? "on" : "off") : "not reporting") + "\n";
    }
    for (const Item &a : d.acs) {
        t += "Air conditioner " + a.name + ": " + (a.on ? "on " + a.mode + (a.temp.empty() ? "" : " to " + a.temp) : "off") + "\n";
    }
    put(out, capacity, t);
}

extern "C" void home_app_command(const char *verb_c, const char *target_c, const char *value_c, char *out, size_t capacity)
{
    using namespace home;
    if (!hub.paired()) {
        return put(out, capacity, "Not paired with Swifty Home.");
    }
    Plan plan;
    if (!plan_command(verb_c ? verb_c : "", target_c ? target_c : "", value_c ? value_c : "", plan)) {
        return put(out, capacity, plan.what);
    }
    for (Planned &p : plan.acts) {
        act(p.action, p.target, std::move(p.params), p.label, p.name);
    }
    put(out, capacity, "Sent: " + plan.what + ". It takes a moment to show.");
}

bool home_app_ready()
{
    using namespace home;
    return G.built && hub.paired() && hub.devices && hub.rooms && !hub.catalog.empty();
}

bool home_app_resolve(const char *verb, const char *target, const char *value, std::string &what, std::string &resolved)
{
    using namespace home;
    if (!hub.paired()) {
        what = "Not paired with Swifty Home.";
        return false;
    }
    Plan plan;
    const bool ok = plan_command(verb ? verb : "", target ? target : "", value ? value : "", plan);
    what = plan.what;
    resolved = plan.resolved;
    return ok;
}

bool home_app_run(const char *verb, const char *target, const char *value, bool dry_run, HomeRunDone done, std::string &what)
{
    using namespace home;
    if (!G.built) {
        what = "Home has not started yet.";
        return false;
    }
    if (!hub.paired()) {
        what = "Not paired with Swifty Home.";
        return false;
    }
    Plan plan;
    if (!plan_command(verb ? verb : "", target ? target : "", value ? value : "", plan)) {
        what = plan.what;
        return false;
    }
    what = plan.what;
    // Nothing is sent unless every part may be.
    for (const Planned &p : plan.acts) {
        const std::string flow = hub.flow(p.action);
        if (!hub.can(p.action)) {
            what = "This display may not do that (" + p.action + ").";
            return false;
        }
        if (flow != "direct" && flow != "prepare_confirm") {
            what = "That needs a passkey (" + p.action + ").";
            return false;
        }
    }
    if (dry_run) {
        std::string sends;
        for (const Planned &p : plan.acts) {
            std::string params;
            serializeJson(p.params, params);
            sends += (sends.empty() ? "" : "; ") + p.action + " " + p.target + " " + params;
        }
        HOME_LOG("dry-run: %s -> %s", plan.what.c_str(), sends.c_str());
        hub.after(300, [done, sends] {
            done(true, "dry-run: " + sends);
        });
        return true;
    }
    // Each part counts once: when the hub reports how it ended. The whole
    // command fails if any part does, or after 15 s without an answer.
    struct State {
        size_t left;
        bool finished = false, failed = false;
        std::string error;
        HomeRunDone done;
    };
    auto st = std::make_shared<State>();
    st->left = plan.acts.size();
    st->done = std::move(done);
    auto finish_one = [st](bool ok, const std::string & error) {
        if (st->finished) {
            return;
        }
        if (!ok && !st->failed) {
            st->failed = true;
            st->error = error;
        }
        if (--st->left == 0) {
            st->finished = true;
            st->done(!st->failed, st->error);
        }
    };
    hub.after(15000, [st] {
        if (!st->finished) {
            st->finished = true;
            st->done(false, "No answer from Swifty Home.");
        }
    });
    for (Planned &p : plan.acts) {
        auto once = std::make_shared<bool>(false);
        const std::string who = p.label, target = p.target, name = p.name;
        RunHandler handler = [finish_one, once, who, target, name](const DocPtr & run, const std::string & error) {
            if (*once) {
                return;
            }
            if (!run) {
                *once = true;
                lean_settle(target, false);
                finish_one(false, error.empty() ? "That did not work." : error);
                return;
            }
            const std::string phase = (*run)["phase"] | "";
            if (phase == "commanded") {
                return;  // still running
            }
            *once = true;
            const bool failed = failed_phase(phase);
            lean_settle(target, !failed);
            finish_one(!failed, failed ? "The " + name + " didn't answer." : std::string());
        };
        if (hub.flow(p.action) == "direct") {
            hub.execute(p.action, p.target, std::move(p.params), handler);
        } else {
            // Asks first (the TV's power, Movie): the shortcut was the tap that confirms.
            hub.prepare(p.action, p.target, std::move(p.params), [handler](const DocPtr & prep, const std::string & error) {
                if (!prep) {
                    handler(nullptr, error);
                } else {
                    hub.confirm(prep, handler);
                }
            });
        }
    }
    return true;
}

std::string home_app_choices_json()
{
    using namespace home;
    JsonDocument doc;
    doc["paired"] = hub.paired();
    doc["ready"] = home_app_ready();
    const Derived d = build();
    JsonArray rs = doc["rooms"].to<JsonArray>();
    for (JsonObjectConst r : rooms()) {
        int lights = 0;
        for (const Item &b : d.bulbs)
            if (room_of(b.target) == r) {
                ++lights;
            }
        JsonObject o = rs.add<JsonObject>();
        o["id"] = r["id"] | "";
        o["name"] = r["name"] | "";
        o["lights"] = lights;
    }
    auto items = [&](const char *key, const std::vector<Item> &list) {
        JsonArray a = doc[key].to<JsonArray>();
        for (const Item &i : list) {
            JsonObject o = a.add<JsonObject>();
            o["target"] = i.target;
            o["name"] = i.name;
            if (key[0] == 'l') {
                o["color"] = i.color;
            }
        }
    };
    items("lights", d.bulbs);
    items("plugs", d.plugs);
    JsonArray ss = doc["scenes"].to<JsonArray>();
    if (hub.scenes)
        for (JsonObjectConst s : (*hub.scenes)["scenes"].as<JsonArrayConst>()) {
            JsonObject o = ss.add<JsonObject>();
            o["id"] = s["id"] | "";
            o["name"] = s["name"] | (s["id"] | "");
        }
    if (d.tv) {
        doc["tv"] = d.tv->name;
    }
    if (d.speaker) {
        doc["speaker"] = d.speaker->name;
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}
