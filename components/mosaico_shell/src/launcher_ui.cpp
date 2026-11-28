// SPDX-License-Identifier: Apache-2.0
#include "launcher_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "launcher_apps.h"
#include "launcher_layout.h"
#include "mui.h"

namespace launcher_brand {
#include "launcher_brand.h"
}

namespace {
using namespace mui;
namespace L = launcher_layout;
constexpr int MIN_RADIUS = 0, MAX_RADIUS = 160;
// Swifty's sky blue (its widget and listening dot); the clock widget's graphite.
const Accent SKY = {0x7DF3FF, 0x2B5CF2, 0x35B2FF, 0x041428, 0x12305A, 0x060E1C};
const Accent GRAPHITE = {0xC9C9CF, 0x5A5A62, 0xB0B0B8, 0x101012, 0x2A2A2E, 0x151517};

// The home screen: a status bar over pages of icons, like a phone's. The first
// page has two live widgets above two rows of icons; the others three rows.
// Hold an icon (or the empty page) to edit: the icons jiggle, drag them about
// (to the edge to turn the page), tap − to hide one. Settings opens as a sheet.
constexpr int ICON = 72, PITCH = 100, GRID_X = 40, ROW = 104, TOP = 12, CELL_H = ROW + TOP;
constexpr int FIRST_Y = 194, PAGE_Y = 64;  // the first row's icon tops: first page, others
constexpr int DOTS_Y = 426, EDGE = 34;     // page dots; the edge strip that turns the page while dragging
int cell_x(int slot)
{
    return GRID_X + (slot % L::COLS) * PITCH;
}
int cell_y(int page, int slot)
{
    return (page ? PAGE_Y : FIRST_Y) - TOP + (slot / L::COLS) * ROW;
}

// One icon on the home screen: an app or a shortcut tile.
struct Cell {
    std::string key;
    const LauncherApp *app = nullptr;  // or a shortcut
    lv_obj_t *obj = nullptr, *tile = nullptr, *minus = nullptr, *mark = nullptr;
    lv_timer_t *mark_timer = nullptr;
    float phase = 0;  // its jiggle's
};

struct Shortcut {
    std::string key, name, glyph;
    const Accent *accent;
    void (*run)(const char *key, void *user);
    void *user;
};

struct {
    LauncherHooks hooks{};
    int radius = 80, draft = 80;
    lv_obj_t *screen = nullptr, *pager = nullptr, *status = nullptr, *dots = nullptr, *editbar = nullptr,
              *settings = nullptr, *hidden_sheet = nullptr;
    lv_obj_t *vpn = nullptr, *vpn_text = nullptr;  // the VPN's badge after the Wi-Fi icon
    lv_obj_t *clock = nullptr, *wifi = nullptr, *date = nullptr, *badge = nullptr, *badge_text = nullptr, *swifty_dot = nullptr;
    lv_obj_t *batt = nullptr, *batt_fill = nullptr, *batt_nub = nullptr, *batt_bolt = nullptr, *picker = nullptr;
    // The pages and their icons
    std::vector<lv_obj_t *> pages, dot_objs;
    std::vector<std::unique_ptr<Cell>> cells;
    std::vector<Shortcut> shortcuts;
    L::Layout layout;
    std::string saved;  // the layout text as last saved
    std::vector<std::string> sheet_keys;  // the hidden apps' sheet shows these
    bool built = false, editing = false;
    int page = 0;  // the page in view (its dot lit)
    lv_timer_t *jiggle = nullptr;
    // A press on an icon, and the icon being dragged
    Cell *pressed = nullptr, *drag = nullptr;
    lv_point_t press_at{}, grab{};
    int drag_page = 0, drag_slot = 0, drag_view = 0, edge_dir = 0;
    uint32_t edge_since = 0, last_turn = 0;
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
               *s_cam_auto = nullptr, *s_web = nullptr, *s_web_hint = nullptr, *s_web_url = nullptr, *s_web_code = nullptr,
                *s_aod = nullptr, *s_hidden = nullptr;
    // Corner calibration
    lv_obj_t *calibration = nullptr, *guide = nullptr, *cal_value = nullptr;
} ui;

void set_color(lv_obj_t *o, uint32_t color)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), lv_color_hex(color))) {
        lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    }
}

void on(lv_obj_t *o, lv_event_cb_t cb, lv_event_code_t code = LV_EVENT_CLICKED, void *data = nullptr)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, cb, code, data);
}

void tick()
{
    if (ui.hooks.tick) {
        ui.hooks.tick();
    }
}

void open_settings()
{
    lv_obj_scroll_to_y(ui.settings, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui.settings);
    stagger(ui.settings, 0, 15);
}

void open_app(int app)
{
    if (app == LAUNCHER_OPEN_SETTINGS) {
        open_settings();
    } else if (app >= 0 && ui.hooks.open) {
        ui.hooks.open(ShellApp(app));
    }
}

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

// ---- the layout: what each key is -----------------------------------------------------------
Shortcut *find_shortcut(const std::string &key)
{
    for (Shortcut &s : ui.shortcuts)
        if (s.key == key) {
            return &s;
        }
    return nullptr;
}

L::Kind kind(const std::string &key)
{
    if (ui.hooks.visible && !ui.hooks.visible(key.c_str())) {
        return L::Kind::Pending;  // not for this profile: keeps its place, takes no cell
    }
    if (launcher_app(key.c_str()) || find_shortcut(key)) {
        return L::Kind::Shown;
    }
    return key.compare(0, 3, "sc:") == 0 ? L::Kind::Pending : L::Kind::Gone;
}

std::vector<std::string> order()
{
    std::vector<std::string> keys;
    for (int i = 0; i < LAUNCHER_APP_COUNT; ++i) {
        keys.push_back(LAUNCHER_APPS[i].key);
    }
    for (const Shortcut &s : ui.shortcuts) {
        keys.push_back(s.key);
    }
    return keys;
}

bool keep(const std::string &key)
{
    const LauncherApp *a = launcher_app(key.c_str());
    return a && a->keep;
}

void normalize()
{
    L::normalize(ui.layout, order(), kind, keep, ui.editing);
}

void save_layout()
{
    L::Layout copy = ui.layout;
    L::normalize(copy, order(), kind, keep, false);
    const std::string text = L::serialize(copy);
    if (text == ui.saved) {
        return;
    }
    ui.saved = text;
    if (ui.hooks.save_layout) {
        ui.hooks.save_layout(text.c_str());
    }
}

Cell *cell(const std::string &key)
{
    for (auto &c : ui.cells)
        if (c->key == key) {
            return c.get();
        }
    return nullptr;
}

// ---- placing icons --------------------------------------------------------------------------
void anim_x(void *o, int32_t v)
{
    lv_obj_set_x(static_cast<lv_obj_t *>(o), v);
}
void anim_y(void *o, int32_t v)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(o), v);
}

// To (x, y) in its parent, gliding there or at once.
void glide(lv_obj_t *o, int x, int y, bool animate)
{
    const int32_t ox = lv_obj_get_style_x(o, LV_PART_MAIN), oy = lv_obj_get_style_y(o, LV_PART_MAIN);
    lv_anim_delete(o, anim_x);
    lv_anim_delete(o, anim_y);
    if (!animate) {
        if (ox != x || oy != y) {
            lv_obj_set_pos(o, x, y);
        }
        return;
    }
    const struct {
        lv_anim_exec_xcb_t cb;
        int32_t from, to;
    } axes[] = {{anim_x, ox, x}, {anim_y, oy, y}};
    for (const auto &axis : axes) {
        if (axis.from == axis.to) {
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_exec_cb(&a, axis.cb);
        lv_anim_set_values(&a, axis.from, axis.to);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

void place(Cell *c, int page, int slot, bool animate)
{
    lv_obj_t *p = ui.pages[size_t(page)];
    if (lv_obj_get_parent(c->obj) != p) {
        lv_obj_set_parent(c->obj, p);
        animate = false;
    }
    // Past a full page (while dragging onto it): off to the next page on the drop.
    set_hidden(c->obj, slot >= L::capacity(page));
    c->phase = float(slot) * 1.9f;
    glide(c->obj, cell_x(slot), cell_y(page, slot), animate);
}

// A page's icons in their slots; while dragging, without the dragged one and
// with a gap where it would land.
void layout_page(int page, bool animate)
{
    if (page < 0 || page >= int(ui.layout.pages.size())) {
        return;
    }
    int slot = 0;
    for (const std::string &key : L::shown(ui.layout.pages[size_t(page)], kind)) {
        Cell *c = cell(key);
        if (!c || c == ui.drag) {
            continue;
        }
        if (ui.drag && page == ui.drag_page && slot == ui.drag_slot) {
            ++slot;
        }
        place(c, page, slot++, animate);
    }
}

void page_dots()
{
    const int n = int(ui.pages.size());
    while (int(ui.dot_objs.size()) > n) {
        lv_obj_delete(ui.dot_objs.back());
        ui.dot_objs.pop_back();
    }
    while (int(ui.dot_objs.size()) < n) {
        lv_obj_t *d = box(ui.dots, 0, 0, 7, 7, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        ui.dot_objs.push_back(d);
    }
    constexpr int GAP = 16;
    const int x0 = PANEL / 2 - ((n - 1) * GAP + 7) / 2;
    for (int i = 0; i < n; ++i) {
        lv_obj_t *d = ui.dot_objs[size_t(i)];
        if (lv_obj_get_style_x(d, LV_PART_MAIN) != x0 + i * GAP) {
            lv_obj_set_x(d, x0 + i * GAP);
        }
        set_bg_color(d, lv_color_hex(i == ui.page ? TEXT : 0x55555B));
    }
    // One page: no dots.
    set_hidden(ui.dots, n < 2);
}

void page_events(lv_obj_t *page);

// The page objects to match the layout's pages: icons on a page that goes
// move to the first one (hidden until placed).
void sync_pages()
{
    const size_t n = std::max<size_t>(1, ui.layout.pages.size());
    while (ui.pages.size() > n) {
        lv_obj_t *p = ui.pages.back();
        for (auto &c : ui.cells)
            if (lv_obj_get_parent(c->obj) == p) {
                lv_obj_add_flag(c->obj, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_parent(c->obj, ui.pages[0]);
            }
        lv_obj_delete(p);
        ui.pages.pop_back();
    }
    while (ui.pages.size() < n) {
        lv_obj_t *p = box(ui.pager, int(ui.pages.size()) * PANEL, 0, PANEL, PANEL);
        page_events(p);
        ui.pages.push_back(p);
    }
}

void apply_layout(bool animate)
{
    sync_pages();
    for (auto &c : ui.cells) {
        if (c.get() == ui.drag) {
            continue;
        }
        int page, slot;
        if (!L::find(ui.layout, c->key, kind, &page, &slot)) {
            lv_obj_add_flag(c->obj, LV_OBJ_FLAG_HIDDEN);  // hidden by the person, or pending
        }
    }
    for (int p = 0; p < int(ui.pages.size()); ++p) {
        layout_page(p, animate);
    }
    const int last = int(ui.pages.size()) - 1;
    if (ui.page > last) {
        ui.page = last;
        lv_obj_scroll_to_x(ui.pager, last * PANEL, LV_ANIM_ON);
    }
    page_dots();
}

// ---- editing --------------------------------------------------------------------------------
void pager_scrolls(bool yes)
{
    if (yes) {
        lv_obj_add_flag(ui.pager, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_remove_flag(ui.pager, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void jiggle_cb(lv_timer_t *)
{
    if (!ui.editing || lv_screen_active() != ui.screen) {
        return;
    }
    // Left alone for a while: still (no redraws for nobody).
    const bool still = lv_display_get_inactive_time(nullptr) > 20000;
    const float t = float(lv_tick_get()) * 0.022f;  // ~3.5 wobbles a second
    for (auto &c : ui.cells) {
        if (c.get() == ui.drag || lv_obj_has_flag(c->obj, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        const lv_obj_t *page = lv_obj_get_parent(c->obj);
        const int p = int(std::find(ui.pages.begin(), ui.pages.end(), page) - ui.pages.begin());
        int dx = 0, dy = 0;
        if (!still && abs(p - ui.page) <= 1) {
            const float a = t + c->phase;
            dx = int(lroundf(1.5f * sinf(a)));
            dy = int(lroundf(1.0f * sinf(2 * a + 1.0f)));
        }
        if (lv_obj_get_style_translate_x(c->obj, LV_PART_MAIN) != dx) {
            lv_obj_set_style_translate_x(c->obj, dx, 0);
        }
        if (lv_obj_get_style_translate_y(c->obj, LV_PART_MAIN) != dy) {
            lv_obj_set_style_translate_y(c->obj, dy, 0);
        }
    }
}

void close_hidden();

void drop();

void set_editing(bool on)
{
    if (on == ui.editing) {
        return;
    }
    if (!on && ui.drag) {  // left mid-drag (an app opened over it): it lands where it was going
        drop();
    }
    ui.editing = on;
    for (auto &c : ui.cells) {
        if (c->minus) {
            set_hidden(c->minus, !on);
        }
        if (!on) {
            lv_obj_set_style_translate_x(c->obj, 0, 0);
            lv_obj_set_style_translate_y(c->obj, 0, 0);
        }
    }
    set_hidden(ui.status, on);
    set_hidden(ui.editbar, !on);
    if (on) {
        lv_timer_resume(ui.jiggle);
    } else {
        lv_timer_pause(ui.jiggle);
    }
    normalize();  // an empty page at the end while editing, to drag onto
    apply_layout(false);
    if (!on) {
        save_layout();
    }
}

void lift(Cell *c, bool up)
{
    // Grown a little, the way a picked-up icon comes forward.
    lv_obj_set_style_transform_width(c->tile, up ? 6 : 0, 0);
    lv_obj_set_style_transform_height(c->tile, up ? 6 : 0, 0);
}

void start_drag(Cell *c)
{
    int page, slot;
    if (ui.drag || !L::find(ui.layout, c->key, kind, &page, &slot)) {
        return;
    }
    ui.drag = c;
    pager_scrolls(false);
    // Off its page onto the screen, where it can go over every page.
    lv_anim_delete(c->obj, anim_x);
    lv_anim_delete(c->obj, anim_y);
    lv_obj_set_style_translate_x(c->obj, 0, 0);
    lv_obj_set_style_translate_y(c->obj, 0, 0);
    lv_area_t pa;
    lv_obj_get_coords(ui.pages[size_t(page)], &pa);
    const int x = pa.x1 + lv_obj_get_style_x(c->obj, LV_PART_MAIN), y = pa.y1 + lv_obj_get_style_y(c->obj, LV_PART_MAIN);
    lv_obj_set_parent(c->obj, ui.screen);
    lv_obj_set_pos(c->obj, x, y);
    lv_obj_move_foreground(c->obj);
    lv_point_t p = ui.press_at;
    if (lv_indev_t *indev = lv_indev_active()) {
        lv_indev_get_point(indev, &p);
    }
    ui.grab = {p.x - x, p.y - y};
    lv_obj_remove_state(c->tile, LV_STATE_PRESSED);
    lift(c, true);
    ui.drag_page = ui.drag_view = page;
    ui.drag_slot = slot;
    ui.edge_dir = 0;
}

void turn_to(int page)
{
    ui.drag_view = page;
    lv_obj_scroll_to_x(ui.pager, page * PANEL, LV_ANIM_ON);
    tick();
}

void drag_move(lv_point_t p)
{
    Cell *c = ui.drag;
    const int x = p.x - ui.grab.x, y = p.y - ui.grab.y;
    lv_obj_set_pos(c->obj, x, y);
    // Held at an edge for a moment: the page turns (and again, after a while there).
    const int n = int(ui.pages.size());
    const int dir = p.x < EDGE ? -1 : p.x >= PANEL - EDGE ? 1 : 0;
    const int want = std::clamp(ui.drag_view + dir, 0, n - 1);
    const uint32_t now = lv_tick_get();
    if (want == ui.drag_view) {
        ui.edge_dir = 0;
    } else if (ui.edge_dir != dir) {
        ui.edge_dir = dir;
        ui.edge_since = now;
    } else if (lv_tick_diff(now, ui.edge_since) > 450 && lv_tick_diff(now, ui.last_turn) > 700) {
        ui.edge_since = ui.last_turn = now;
        turn_to(want);
    }
    // The slot under the icon's centre, on the page in view.
    const int page = ui.drag_view;
    const int rows = L::capacity(page) / L::COLS;
    const int cx = x + PITCH / 2, cy = y + TOP + ICON / 2;
    const int col = std::clamp(int(floorf(float(cx - GRID_X) / PITCH)), 0, L::COLS - 1);
    const int row = std::clamp(int(floorf(float(cy - cell_y(page, 0)) / ROW)), 0, rows - 1);
    int count = 0;
    for (const std::string &key : L::shown(ui.layout.pages[size_t(page)], kind))
        if (key != c->key) {
            ++count;
        }
    const int slot = std::min(row * L::COLS + col, std::min(count, L::capacity(page) - 1));
    if (page != ui.drag_page || slot != ui.drag_slot) {
        const int was = ui.drag_page;
        ui.drag_page = page;
        ui.drag_slot = slot;
        if (was != page) {
            layout_page(was, true);
        }
        layout_page(page, true);
    }
}

void drop()
{
    Cell *c = ui.drag;
    ui.drag = nullptr;
    L::move(ui.layout, c->key, ui.drag_page, ui.drag_slot, kind);
    normalize();
    sync_pages();
    lv_obj_update_layout(ui.screen);
    // Back onto its page from where the finger left it, then home.
    int page = 0, slot = 0;
    L::find(ui.layout, c->key, kind, &page, &slot);
    lv_area_t pa;
    lv_obj_get_coords(ui.pages[size_t(page)], &pa);
    const int x = lv_obj_get_style_x(c->obj, LV_PART_MAIN), y = lv_obj_get_style_y(c->obj, LV_PART_MAIN);
    lv_obj_set_parent(c->obj, ui.pages[size_t(page)]);
    lv_obj_set_pos(c->obj, x - pa.x1, y - pa.y1);
    lift(c, false);
    apply_layout(true);
    pager_scrolls(true);
    save_layout();
}

void hide_cb(lv_event_t *e)
{
    Cell *c = static_cast<Cell *>(lv_event_get_user_data(e));
    if (!ui.editing || ui.drag) {
        return;
    }
    L::hide(ui.layout, c->key);
    normalize();
    apply_layout(true);
    save_layout();
}

void open_item(Cell *c)
{
    if (c->app) {
        open_app(c->app->open);
    } else if (Shortcut *s = find_shortcut(c->key)) {
        if (s->run) {
            s->run(s->key.c_str(), s->user);
        }
    }
}

void tile_cb(lv_event_t *e)
{
    Cell *c = static_cast<Cell *>(lv_event_get_user_data(e));
    lv_point_t p{};
    if (lv_indev_t *indev = lv_indev_active()) {
        lv_indev_get_point(indev, &p);
    }
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        ui.pressed = c;
        ui.press_at = p;
        if (ui.editing) {
            pager_scrolls(false);  // a drag moves the icon, not the page
        }
        break;
    case LV_EVENT_LONG_PRESSED:
        if (!ui.editing) {
            set_editing(true);
            tick();
        }
        start_drag(c);
        break;
    case LV_EVENT_PRESSING:
        if (ui.drag == c) {
            drag_move(p);
        } else if (ui.editing && ui.pressed == c && !ui.drag &&
                   abs(p.x - ui.press_at.x) + abs(p.y - ui.press_at.y) > 12) {
            start_drag(c);
            drag_move(p);
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (ui.drag == c) {
            drop();
        }
        ui.pressed = nullptr;
        if (!ui.drag) {
            pager_scrolls(true);
        }
        break;
    case LV_EVENT_SHORT_CLICKED:
        if (!ui.editing) {
            open_item(c);
        }
        break;
    default:
        break;
    }
}

// The empty page: hold to edit, tap to finish.
void page_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        if (!ui.editing) {
            set_editing(true);
            tick();
        }
    } else if (ui.editing) {
        set_editing(false);
    }
}

void page_events(lv_obj_t *p)
{
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, page_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(p, page_cb, LV_EVENT_SHORT_CLICKED, nullptr);
}

void scroll_cb(lv_event_t *)
{
    const int page = std::clamp(int(lroundf(float(lv_obj_get_scroll_x(ui.pager)) / PANEL)), 0,
                                std::max(0, int(ui.pages.size()) - 1));
    if (page != ui.page) {
        ui.page = page;
        page_dots();
    }
}

// ---- icons ----------------------------------------------------------------------------------
// An icon's look: the tile and its label in a cell of PITCH x CELL_H, with room
// above the tile for a badge (LVGL redraws a child only inside its parents: a
// badge poking out of the cell was cut off whenever it changed).
lv_obj_t *draw_item(lv_obj_t *parent, const std::string &key, lv_obj_t **tile_out)
{
    lv_obj_t *o = box(parent, 0, 0, PITCH, CELL_H);
    lv_obj_t *t = box(o, (PITCH - ICON) / 2, TOP, ICON, ICON, 20);
    if (const LauncherApp *a = launcher_app(key.c_str())) {
        style_fill(t, *a->accent, false);
        if (a->glyph) {
            icon(t, a->glyph, 16, 16, 40, 0xFFFFFF, 2.0f);
        } else {
            lv_obj_add_event_cb(t, draw_g, LV_EVENT_DRAW_MAIN, nullptr);
        }
        label(o, a->name, 0, TOP + ICON + 6, PITCH, TEXT, 13, true, LV_TEXT_ALIGN_CENTER);
    } else if (const Shortcut *s = find_shortcut(key)) {
        // A shortcut: a glass tile with the glyph in its colour.
        const Accent &a = s->accent ? *s->accent : VIOLET;
        style_glass(t, a);
        lv_obj_set_style_border_color(t, lv_color_hex(a.glass_a), 0);
        icon(t, s->glyph.c_str(), 18, 18, 36, a.glow, 2.2f);
        label(o, s->name.c_str(), 0, TOP + ICON + 6, PITCH, TEXT, 13, true, LV_TEXT_ALIGN_CENTER);
    }
    *tile_out = t;
    return o;
}

Cell *make_cell(const std::string &key)
{
    auto owned = std::make_unique<Cell>();
    Cell *c = owned.get();
    c->key = key;
    c->app = launcher_app(key.c_str());
    c->obj = draw_item(ui.pages[0], key, &c->tile);
    lv_obj_add_flag(c->obj, LV_OBJ_FLAG_HIDDEN);
    press_fx(c->tile);
    lv_obj_add_flag(c->tile, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    for (lv_event_code_t code : {
                LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_LONG_PRESSED, LV_EVENT_RELEASED,
                LV_EVENT_PRESS_LOST, LV_EVENT_SHORT_CLICKED
            }) {
        lv_obj_add_event_cb(c->tile, tile_cb, code, c);
    }
    if (key == "glimdock") {  // a badge with the alert count
        ui.badge = box(c->obj, (PITCH + ICON) / 2 - 16, TOP - 10, 26, 26, 13);
        lv_obj_set_style_bg_color(ui.badge, lv_color_hex(0xFF3B30), 0);
        lv_obj_set_style_bg_opa(ui.badge, LV_OPA_COVER, 0);
        ui.badge_text = label(ui.badge, "", 0, 4, 26, 0xFFFFFF, 13, true, LV_TEXT_ALIGN_CENTER);
        lv_obj_add_flag(ui.badge, LV_OBJ_FLAG_HIDDEN);
    } else if (key == "swifty") {  // listening or thinking
        ui.swifty_dot = box(c->obj, (PITCH + ICON) / 2 - 10, TOP - 4, 14, 14, 7);
        lv_obj_set_style_bg_color(ui.swifty_dot, lv_color_hex(SKY.glow), 0);
        lv_obj_set_style_bg_opa(ui.swifty_dot, LV_OPA_COVER, 0);
        lv_obj_add_flag(ui.swifty_dot, LV_OBJ_FLAG_HIDDEN);
    }
    // While editing: − at the tile's corner hides it (not Settings, the way back).
    if (!(c->app && c->app->keep)) {
        c->minus = box(c->obj, (PITCH - ICON) / 2 - 12, TOP - 12, 34, 34);
        lv_obj_t *d = box(c->minus, 4, 4, 26, 26, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x4A4A50), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 2, 0);
        lv_obj_set_style_border_color(d, lv_color_black(), 0);
        lv_obj_t *bar = box(d, 7, 11, 12, 3, 1);
        lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        on(c->minus, hide_cb, LV_EVENT_CLICKED, c);
        set_hidden(c->minus, !ui.editing);
    }
    ui.cells.push_back(std::move(owned));
    return c;
}

void delete_cell(const std::string &key)
{
    for (auto it = ui.cells.begin(); it != ui.cells.end(); ++it) {
        Cell *c = it->get();
        if (c->key != key) {
            continue;
        }
        if (ui.drag == c) {
            ui.drag = nullptr;
            pager_scrolls(true);
        }
        if (ui.pressed == c) {
            ui.pressed = nullptr;
        }
        if (c->mark_timer) {
            lv_timer_delete(c->mark_timer);
        }
        lv_obj_delete(c->obj);
        ui.cells.erase(it);
        return;
    }
}

// ---- hidden apps ----------------------------------------------------------------------------
void open_hidden();

void close_hidden()
{
    if (ui.hidden_sheet) {
        lv_obj_delete_async(ui.hidden_sheet);
    }
    ui.hidden_sheet = nullptr;
}

void unhide_cb(lv_event_t *e)
{
    const size_t i = size_t(intptr_t(lv_event_get_user_data(e)));
    if (i >= ui.sheet_keys.size()) {
        return;
    }
    const std::string key = ui.sheet_keys[i];
    L::unhide(ui.layout, key, ui.page, kind);
    normalize();
    apply_layout(false);
    save_layout();
    open_hidden();  // the rest, still hidden
}

void reset_cb(lv_event_t *e)
{
    // Twice: the first tap asks.
    lv_obj_t *b = lv_event_get_current_target_obj(e);
    lv_obj_t *text = lv_obj_get_child(b, 0);
    if (!lv_obj_has_state(b, LV_STATE_CHECKED)) {
        lv_obj_add_state(b, LV_STATE_CHECKED);
        set_text(text, "Tap again to reset");
        set_text_color(text, lv_color_hex(WARN));
        return;
    }
    ui.layout = L::Layout{};
    normalize();
    apply_layout(false);
    save_layout();
    open_hidden();
}

void open_hidden()
{
    close_hidden();
    lv_obj_t *o = box(ui.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    ui.hidden_sheet = o;
    lv_obj_t *back = box(o, 56, 14, 44, 44, 22);
    lv_obj_set_style_bg_color(back, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    icon(back, "back", 10, 10, 24, TEXT, 2.0f);
    press_fx(back);
    on(back, [](lv_event_t *) {
        close_hidden();
    });
    label(o, "Hidden apps", 112, 18, 0, TEXT, 30, true);
    ui.sheet_keys = ui.layout.hidden;
    ui.sheet_keys.erase(std::remove_if(ui.sheet_keys.begin(), ui.sheet_keys.end(), [](const std::string & k) {
        return ui.hooks.visible && !ui.hooks.visible(k.c_str());
    }), ui.sheet_keys.end());
    const std::vector<std::string> &hidden = ui.sheet_keys;
    if (hidden.empty()) {
        lv_obj_t *t = label(o, "Nothing hidden. Hold an icon on the home screen, then tap \xE2\x88\x92 to hide it.",
                            70, 150, 340, TEXT2, 16, false, LV_TEXT_ALIGN_CENTER);
        wrap_label(t);
    } else {
        label(o, "Tap one to put it back on the home screen.", 40, 72, 400, TEXT3, 14, false, LV_TEXT_ALIGN_CENTER);
        for (size_t i = 0; i < hidden.size(); ++i) {
            lv_obj_t *tile;
            lv_obj_t *c = draw_item(o, hidden[i], &tile);
            lv_obj_set_pos(c, cell_x(int(i)), 100 + int(i / L::COLS) * ROW);
            // A + at its corner says what a tap does.
            lv_obj_t *plus = box(c, (PITCH - ICON) / 2 - 8, TOP - 8, 26, 26, LV_RADIUS_CIRCLE);
            lv_obj_set_style_bg_color(plus, lv_color_hex(GREEN.glow), 0);
            lv_obj_set_style_bg_opa(plus, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(plus, 2, 0);
            lv_obj_set_style_border_color(plus, lv_color_black(), 0);
            icon(plus, "plus", 3, 3, 16, 0x06200F, 2.6f);
            press_fx(tile);
            on(tile, unhide_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(i)));
        }
    }
    const int rows = std::max(1, int((hidden.size() + L::COLS - 1) / L::COLS));
    const int by = std::max(370, 100 + rows * ROW + 24);
    lv_obj_t *reset = field_button(o, ui.hooks.shortcuts ? 46 : 130, by, ui.hooks.shortcuts ? 200 : 220, 46,
                                   "Reset home screen", 15, TEXT2);
    on(reset, reset_cb);
    if (ui.hooks.shortcuts) {  // where new shortcut tiles come from
        lv_obj_t *sc = fill_button(o, 258, by, 176, 46, "Shortcuts", VIOLET, 15);
        on(sc, [](lv_event_t *) {
            close_hidden();
            ui.hooks.shortcuts();
        });
    }
    box(o, 0, by + 46, 1, 70);  // room past the rounded bottom
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    stagger(o, 0, 12);
}

// The bar over the status bar while editing: + (the hidden apps) and Done.
void build_editbar()
{
    ui.editbar = box(ui.screen, 0, 0, PANEL, 48);
    lv_obj_t *plus = field_button(ui.editbar, 52, 8, 44, 34, "", 16);
    icon(plus, "plus", 10, 5, 24, TEXT, 2.2f);
    on(plus, [](lv_event_t *) {
        open_hidden();
    });
    label(ui.editbar, "Drag to move \xC2\xB7 \xE2\x88\x92 hides", 104, 16, 232, TEXT2, 13, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *done = fill_button(ui.editbar, PANEL - 52 - 88, 8, 88, 34, "Done", GREEN, 15);
    on(done, [](lv_event_t *) {
        set_editing(false);
    });
    lv_obj_add_flag(ui.editbar, LV_OBJ_FLAG_HIDDEN);
}

// ---- widgets ---------------------------------------------------------------------------------
// Two slots above the icons; hold one to choose what it shows.
enum WidgetKind { W_GLIMDOCK, W_LIGHTS, W_MUSIC, W_SWIFTY, W_BATTERY, W_CLOCK, W_KINDS };
const char *const WIDGET_NAMES[W_KINDS] = {"Glimdock", "Lights", "Music", "Swifty", "Battery", "Clock"};
const char *const WIDGET_ICONS[W_KINDS] = {"tech", "bulb", "speaker", "mic", "power", "clock"};
const int WIDGET_APP[W_KINDS] = {int(ShellApp::Glimdock), int(ShellApp::Home), int(ShellApp::Home),
                                 int(ShellApp::Swifty), -3, -3
                                };
constexpr int WX = 40, WY = 44, WW = 196, WH = 136, WGAP = 8;

void update_widget(int slot);
void open_picker(int slot);

void widget_tap_cb(lv_event_t *e)
{
    const int slot = int(intptr_t(lv_event_get_user_data(e)));
    if (ui.editing) {  // while editing, a widget picks what it shows
        open_picker(slot);
        return;
    }
    const int app = WIDGET_APP[ui.w[slot].kind];
    if (app >= 0) {
        open_app(app);
    }
}

void widget_hold_cb(lv_event_t *e)
{
    open_picker(int(intptr_t(lv_event_get_user_data(e))));
}

void build_widget(int slot)
{
    auto &w = ui.w[slot];
    if (w.card) {
        lv_obj_delete(w.card);
    }
    const int kind = w.kind;
    const Accent &acc = kind == W_GLIMDOCK ? CYAN : kind == W_SWIFTY ? SKY : kind == W_BATTERY ? GREEN
                        : kind == W_CLOCK ? GRAPHITE : VIOLET;
    w = {kind, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    w.card = glass(ui.pages[0], WX + slot * (WW + WGAP), WY, WW, WH, 26, acc);
    press_fx(w.card);
    lv_obj_add_flag(w.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(w.card, widget_tap_cb, LV_EVENT_SHORT_CLICKED, reinterpret_cast<void *>(intptr_t(slot)));
    lv_obj_add_event_cb(w.card, widget_hold_cb, LV_EVENT_LONG_PRESSED, reinterpret_cast<void *>(intptr_t(slot)));
    if (kind == W_LIGHTS || kind == W_MUSIC || kind == W_BATTERY) {
        card_fill(w.card, 0, 0xFFD9A0, 0xF3B56A);
    }
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
    if (ui.have_data) {
        update_widget(slot);
    }
}

// Ink that stays readable over a card's fill.
void ink(int slot, bool dark)
{
    auto &w = ui.w[slot];
    const uint32_t c = 0x2A1A05;
    if (w.icon) {
        lv_obj_set_style_line_color(w.icon, lv_color_hex(dark ? c : TEXT), 0);
    }
    set_color(w.title, dark ? c : TEXT2);
    if (w.big) {
        set_color(w.big, dark ? c : TEXT);
    }
    set_color(w.line1, dark ? c : TEXT2);
    if (w.line2) {
        set_color(w.line2, dark ? c : TEXT3);
    }
}

void update_widget(int slot)
{
    auto &w = ui.w[slot];
    char v[64];
    switch (w.kind) {
    case W_GLIMDOCK: {
        const glimdock_summary_t &g = ui.glim;
        if (isfinite(g.cpu)) {
            snprintf(v, sizeof(v), "%d%% CPU", int(lroundf(g.cpu)));
        } else {
            strlcpy(v, g.started ? "\xE2\x80\x94" : "Homelab", sizeof(v));
        }
        set_text(w.big, v);
        if (isfinite(g.memory)) {
            snprintf(v, sizeof(v), "Memory %d%%", int(lroundf(g.memory)));
        } else {
            strlcpy(v, g.status, sizeof(v));
        }
        set_text(w.line1, v);
        set_bg_color(w.dot, lv_color_hex(!g.ready ? OFF : g.alerts ? RED : OK));
        float top = 40;
        for (int i = 0; i < g.history_count; ++i) {
            top = std::max(top, g.history[i] * 1.25f);
        }
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
        if (!h.paired) {
            strlcpy(v, h.started ? "Pair" : "Home", sizeof(v));
        } else if (h.lights && h.lights_on) {
            snprintf(v, sizeof(v), "%d%%", h.lights_pct);
        } else {
            strlcpy(v, h.lights ? "Off" : "\xE2\x80\x94", sizeof(v));
        }
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
        if (h.music && h.volume >= 0) {
            snprintf(v, sizeof(v), "%d", h.volume);
        } else {
            strlcpy(v, "\xE2\x80\x94", sizeof(v));
        }
        set_text(w.big, v);
        set_text(w.line1, h.music ? h.music_state : h.paired ? "No speaker" : "Open Home to pair");
        set_text(w.line2, h.music ? "Volume" : "");
        break;
    }
    case W_SWIFTY: {
        std::string plain;
        for (const char *c = ui.swifty.last; *c; ++c)
            if (*c != '*' && *c != '`' && *c != '#') {
                plain += *c == '\n' ? ' ' : *c;
            }
        set_text(w.line1, plain.empty() ? (ui.swifty.configured ? "Hold the AI key and ask me anything." : "Set me up in Settings, Web settings.")
                 : plain.c_str());
        set_text(w.title, ui.swifty.listening ? "Listening\xE2\x80\xA6" : ui.swifty.busy ? "Thinking\xE2\x80\xA6" : "Swifty");
        break;
    }
    case W_BATTERY: {
        const int b = ui.battery;
        card_fill_set(w.card, b > 0 ? b : 0, b >= 0 && b <= 20 ? RED : GREEN.glow, b >= 0 && b <= 20 ? RED : GREEN.glow);
        ink(slot, b > 45);
        if (b >= 0) {
            snprintf(v, sizeof(v), "%d%%", b);
        } else {
            strlcpy(v, "\xE2\x80\x94", sizeof(v));
        }
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
    if (ui.picker) {
        lv_obj_delete_async(ui.picker);
    }
    ui.picker = nullptr;
}

void pick_cb(lv_event_t *e)
{
    const int v = int(intptr_t(lv_event_get_user_data(e)));
    const int slot = v >> 8, kind = v & 0xFF;
    close_picker();
    ui.w[slot].kind = kind;
    build_widget(slot);
    if (ui.hooks.save_widgets) {
        ui.hooks.save_widgets(ui.w[0].kind, ui.w[1].kind);
    }
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
        if (k == ui.w[slot].kind) {
            icon(r, "check", 146, 16, 24, CYAN.glow, 2.0f);
        }
        press_fx(r);
        on(r, pick_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(slot << 8 | k)));
    }
    lv_obj_t *cancel = field_button(o, 170, 366, 140, 46, "Cancel", 16, TEXT2);
    on(cancel, [](lv_event_t *) {
        close_picker();
    });
    stagger(o, 0, 12);
}

// ---- status bar: clock, date, Wi-Fi, battery ---------------------------------------------------
// How far in the rounded corner reaches at height y (the glass's radius).
int corner_inset(int y)
{
    const int r = ui.radius;
    if (r <= 0 || y >= r) {
        return 0;
    }
    const float d = float(r - y);
    return int(ceilf(float(r) - sqrtf(float(r * r) - d * d)));
}

void build_status_bar()
{
    // Out towards the corners, as far as the rounded glass allows at this
    // height: the clock and Wi-Fi on the left, the battery on the right.
    ui.status = box(ui.screen, 0, 0, PANEL, 40);
    const int edge = std::max(20, corner_inset(14) + 10);
    ui.clock = label(ui.status, "--:--", edge, 12, 60, TEXT, 15, true);
    ui.wifi = label(ui.status, LV_SYMBOL_WIFI, edge + 50, 12, 24, TEXT, 15, true);
    // The VPN: "VPN" in a rounded outline, as phones show it, while it is on.
    ui.vpn = box(ui.status, edge + 76, 13, 32, 17, 5);
    lv_obj_set_style_border_width(ui.vpn, 1, 0);
    lv_obj_set_style_border_color(ui.vpn, lv_color_hex(TEXT), 0);
    ui.vpn_text = label(ui.vpn, "VPN", 0, 0, 30, TEXT, 11, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(ui.vpn, LV_OBJ_FLAG_HIDDEN);
    ui.date = label(ui.status, "", 150, 12, 180, TEXT2, 15, true, LV_TEXT_ALIGN_CENTER);
    // The battery: an outline with a fill coloured by charge, and a bolt while charging.
    const int bx = PANEL - edge - 33;
    ui.batt = box(ui.status, bx, 14, 30, 15, 4);
    lv_obj_set_style_border_width(ui.batt, 2, 0);
    lv_obj_set_style_border_color(ui.batt, lv_color_hex(TEXT2), 0);
    ui.batt_fill = box(ui.batt, 1, 1, 20, 9, 2);
    lv_obj_set_style_bg_color(ui.batt_fill, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(ui.batt_fill, LV_OPA_COVER, 0);
    ui.batt_nub = box(ui.status, bx + 31, 19, 3, 6, 1);
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
    set_hidden(ui.batt, !known);
    set_hidden(ui.batt_nub, !known);
    if (!known) {
        return;
    }
    // No number: the colour says it. Green charging or plenty, amber under
    // half, red under a fifth.
    const int pct = std::clamp(b, 0, 100);
    const uint32_t color = ui.charging ? OK : pct <= 20 ? RED : pct <= 50 ? WARN : OK;
    const int width = std::max(3, 24 * pct / 100);
    if (lv_obj_get_style_width(ui.batt_fill, LV_PART_MAIN) != width) {
        lv_obj_set_width(ui.batt_fill, width);
    }
    set_bg_color(ui.batt_fill, lv_color_hex(color));
    if (!lv_color_eq(lv_obj_get_style_border_color(ui.batt, LV_PART_MAIN), lv_color_hex(ui.charging ? OK : TEXT2))) {
        lv_obj_set_style_border_color(ui.batt, lv_color_hex(ui.charging ? OK : TEXT2), 0);
    }
    set_bg_color(ui.batt_nub, lv_color_hex(ui.charging ? OK : TEXT2));
    set_hidden(ui.batt_bolt, !ui.charging);
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
    if (ui.hooks.brightness) {
        ui.hooks.brightness(v, release);
    }
}

void haptics_cb(lv_event_t *e)
{
    if (ui.hooks.haptics) {
        ui.hooks.haptics(toggle_on(lv_event_get_current_target_obj(e)));
    }
}

void rotate_cb(lv_event_t *e)
{
    if (ui.hooks.auto_rotate) {
        ui.hooks.auto_rotate(toggle_on(lv_event_get_current_target_obj(e)));
    }
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
    on(back, [](lv_event_t *) {
        lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
    });
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

    // Home screen: the hidden apps (and the way to put them back).
    lv_obj_t *hs = glass(c, 10, 192, 460, 86, 26, GREEN);
    label(hs, "Home screen", 24, 16, 260, TEXT, 18, true);
    label(hs, "Hold an icon to move or hide it", 24, 44, 280, TEXT3, 14);
    ui.s_hidden = label(hs, "", 250, 30, 150, TEXT2, 15, true, LV_TEXT_ALIGN_RIGHT);
    icon(hs, "chev", 408, 30, 24, TEXT2, 2.0f);
    press_fx(hs);
    on(hs, [](lv_event_t *) {
        open_hidden();
    });

    constexpr int Y = 96;  // the cards below Home screen
    // Always-on display: a dim clock after this long without a touch or the AI key.
    lv_obj_t *aod = glass(c, 10, Y + 192, 460, 124, 28, GREEN);
    label(aod, "Always-on display", 24, 16, 300, TEXT, 18, true);
    label(aod, "A dim clock on black after this long unused", 24, 42, 412, TEXT3, 14);
    static const char *const AOD_LABELS[ALWAYS_ON_COUNT] = {"Off", "30 s", "1 min", "2 min", "5 min", "10 min"};
    ui.s_aod = capsule(aod, 16, 70, 428, 40, AOD_LABELS, ALWAYS_ON_COUNT, -1, GREEN, 14);
    lv_obj_add_event_cb(ui.s_aod, [](lv_event_t *e) {
        const int i = capsule_selected(lv_event_get_current_target_obj(e));
        if (ui.hooks.always_on && i >= 0 && i < ALWAYS_ON_COUNT) {
            ui.hooks.always_on(ALWAYS_ON_CHOICES[i]);
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *hp = glass(c, 10, Y + 328, 224, 112, 26, GREEN);
    // Two switches: touch ticks, and turning the UI with the board.
    label(hp, "Haptics", 22, 19, 120, TEXT, 17, true);
    lv_obj_t *t = toggle(hp, 146, 14, haptics, GREEN);
    lv_obj_add_event_cb(t, haptics_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    label(hp, "Auto-rotate", 22, 67, 120, TEXT, 17, true);
    t = toggle(hp, 146, 62, auto_rotate, GREEN);
    lv_obj_add_event_cb(t, rotate_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *cr = glass(c, 246, Y + 328, 224, 112, 26, GREEN);
    label(cr, "Corners", 22, 16, 170, TEXT, 18, true);
    label(cr, "Glass radius", 22, 42, 170, TEXT3, 14);
    lv_obj_t *minus = field_button(cr, 20, 68, 48, 34, "\xE2\x88\x92", 20);
    on(minus, corners_step_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(-4)));
    snprintf(text, sizeof(text), "%d", ui.radius);
    ui.s_radius = label(cr, text, 68, 69, 88, TEXT, 24, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *plus = field_button(cr, 156, 68, 48, 34, "+", 20);
    on(plus, corners_step_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(4)));

    lv_obj_t *wf = glass(c, 10, Y + 452, 460, 112, 28, GREEN);
    ui.s_wifi_dot = dot(wf, 26, 24, 10, OFF, false);
    ui.s_ssid = label(wf, "Wi-Fi", 46, 16, 210, TEXT, 18, true);
    lv_label_set_long_mode(ui.s_ssid, LV_LABEL_LONG_MODE_DOTS);
    ui.s_wifi_state = label(wf, "", 262, 18, 150, TEXT2, 15, true, LV_TEXT_ALIGN_RIGHT);
    if (ui.hooks.wifi) {  // the card opens Settings > Wi-Fi
        icon(wf, "chev", 418, 16, 22, TEXT3, 1.8f);
        lv_obj_add_flag(wf, LV_OBJ_FLAG_CLICKABLE);
        press_fx(wf);
        on(wf, [](lv_event_t *) {
            if (ui.hooks.wifi) {
                ui.hooks.wifi();
            }
        }, LV_EVENT_CLICKED, nullptr);
    }
    ui.s_detail = label(wf, "", 26, 48, 410, TEXT2, 15);
    ui.s_versions = label(wf, "", 26, 74, 380, TEXT3, 14);

    // Time zone: automatic (from the network's location) or one of the clock's zones.
    lv_obj_t *tz = glass(c, 10, Y + 576, 460, 112, 28, GREEN);
    label(tz, "Time zone", 24, 16, 200, TEXT, 18, true);
    label(tz, "Clocks in every app", 24, 42, 220, TEXT3, 14);
    ui.s_zone = label(tz, "", 24, 72, 250, TEXT2, 16, true);
    lv_obj_t *prev = field_button(tz, 296, 56, 64, 40, "", 16);
    icon(prev, "back", 20, 8, 24, TEXT, 2.0f);
    on(prev, [](lv_event_t *) {
        if (ui.hooks.time_zone) {
            ui.hooks.time_zone(-1);
        }
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *next = field_button(tz, 372, 56, 64, 40, "", 16);
    icon(next, "chev", 20, 8, 24, TEXT, 2.0f);
    on(next, [](lv_event_t *) {
        if (ui.hooks.time_zone) {
            ui.hooks.time_zone(1);
        }
    }, LV_EVENT_CLICKED, nullptr);
    // Camera: front/rear found automatically, or only by its button.
    lv_obj_t *cam = glass(c, 10, Y + 700, 460, 96, 28, GREEN);
    label(cam, "Camera", 24, 16, 260, TEXT, 18, true);
    label(cam, "Find front or rear automatically", 24, 44, 300, TEXT3, 14);
    label(cam, "Off: only the camera's button switches", 24, 64, 300, TEXT3, 14);
    ui.s_cam_auto = toggle(cam, 360, 28, true, GREEN);
    lv_obj_add_event_cb(ui.s_cam_auto, [](lv_event_t *e) {
        if (ui.hooks.camera_auto) {
            ui.hooks.camera_auto(toggle_on(lv_event_get_current_target_obj(e)));
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    // Web settings: URLs and keys from a browser, only while switched on.
    lv_obj_t *web = glass(c, 10, Y + 808, 460, 132, 28, GREEN);
    label(web, "Web settings", 24, 16, 260, TEXT, 18, true);
    ui.s_web_hint = label(web, "", 24, 44, 320, TEXT3, 14);
    ui.s_web_url = label(web, "", 24, 68, 412, TEXT, 18, true);
    ui.s_web_code = label(web, "", 24, 96, 412, TEXT2, 16, true);
    ui.s_web = toggle(web, 360, 22, false, GREEN);
    lv_obj_add_event_cb(ui.s_web, [](lv_event_t *e) {
        if (ui.hooks.web_settings) {
            ui.hooks.web_settings(toggle_on(lv_event_get_current_target_obj(e)));
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    box(c, 0, Y + 940, 1, 60);  // room to scroll the last card clear of the rounded bottom
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
}

void update_settings(const LauncherStatus &s)
{
    const uint32_t color = s.wifi_connected ? OK : s.wifi_configured ? WARN : RED;
    set_bg_color(ui.s_wifi_dot, lv_color_hex(color));
    set_text(ui.s_ssid, s.ssid && *s.ssid ? s.ssid : "Wi-Fi");
    set_text(ui.s_wifi_state, s.wifi_connected ? "Connected" : s.wifi_configured ? "Connecting\xE2\x80\xA6" : "Not set");
    set_color(ui.s_wifi_state, color);
    set_text(ui.s_detail, s.wifi_connected && s.ip ? s.ip : s.wifi_configured ? "Joining the network\xE2\x80\xA6"
             : "Tap to choose a network");
    set_text(ui.s_versions, s.versions ? s.versions : "");
    set_text(ui.s_zone, s.zone ? s.zone : "UTC");
    if (ui.s_cam_auto && toggle_on(ui.s_cam_auto) != s.camera_auto) {
        toggle_set(ui.s_cam_auto, s.camera_auto, false);
    }
    if (ui.s_aod && s.always_on >= 0) {
        const int *end = ALWAYS_ON_CHOICES + ALWAYS_ON_COUNT;
        const int i = int(std::find(ALWAYS_ON_CHOICES, end, s.always_on) - ALWAYS_ON_CHOICES);
        const int shown = capsule_selected(ui.s_aod);
        if (i < ALWAYS_ON_COUNT && shown != i) {
            capsule_select(ui.s_aod, i, shown >= 0);
        }
    }
    if (ui.s_web) {
        if (toggle_on(ui.s_web) != s.web_on) {
            toggle_set(ui.s_web, s.web_on, true);
        }
        char code[24] = "";
        if (s.web_on && s.web_code && strlen(s.web_code) == 6) {
            snprintf(code, sizeof(code), "Code  %.3s %.3s", s.web_code, s.web_code + 3);
        }
        const bool noted = !s.web_on && s.web_note && *s.web_note;
        set_text(ui.s_web_hint, s.web_on ? "Open in a browser on this network" : "Edit URLs and keys from a browser");
        set_text(ui.s_web_url, s.web_on ? s.web_url : noted ? s.web_note : "Off");
        set_text_color(ui.s_web_url, lv_color_hex(s.web_on ? TEXT : noted ? WARN : TEXT3));
        set_text(ui.s_web_code, code);
    }
    char hidden[24];
    const size_t n = ui.layout.hidden.size();
    if (n) {
        snprintf(hidden, sizeof(hidden), "%u hidden", unsigned(n));
    }
    set_text(ui.s_hidden, n ? hidden : "None hidden");
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
    if (ui.hooks.save_radius) {
        ui.hooks.save_radius(ui.draft);
    }
}

void cancel_cb(lv_event_t *)
{
    launcher_ui_calibration(false, ui.radius);
}

// ---- shortcut tiles' feedback ----------------------------------------------------------------
void mark_clear(lv_timer_t *t)
{
    Cell *c = static_cast<Cell *>(lv_timer_get_user_data(t));
    c->mark_timer = nullptr;
    if (c->mark) {
        lv_obj_delete(c->mark);
        c->mark = nullptr;
    }
}
}  // namespace

lv_obj_t *launcher_ui_build(const LauncherHooks &hooks, int radius, int brightness, bool haptics, bool auto_rotate,
                            const lv_image_dsc_t *portrait, int left_widget, int right_widget, const char *layout)
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
    // The pages side by side; a swipe turns one at a time, snapping.
    ui.pager = box(s, 0, 0, PANEL, PANEL);
    lv_obj_add_flag(ui.pager, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scroll_dir(ui.pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(ui.pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(ui.pager, LV_SCROLLBAR_MODE_OFF);
    pager_scrolls(true);
    lv_obj_add_event_cb(ui.pager, scroll_cb, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(ui.pager, scroll_cb, LV_EVENT_SCROLL_END, nullptr);
    ui.layout = L::parse(layout ? layout : "");
    ui.saved = layout ? layout : "";
    normalize();
    sync_pages();
    build_status_bar();
    ui.dots = box(s, 0, DOTS_Y, PANEL, 7);
    build_editbar();
    ui.w[0].kind = left_widget >= 0 && left_widget < W_KINDS ? left_widget : W_GLIMDOCK;
    ui.w[1].kind = right_widget >= 0 && right_widget < W_KINDS ? right_widget : W_LIGHTS;
    build_widget(0);
    build_widget(1);
    for (int i = 0; i < LAUNCHER_APP_COUNT; ++i) {
        make_cell(LAUNCHER_APPS[i].key);
    }
    for (const Shortcut &sc : ui.shortcuts) {
        make_cell(sc.key);
    }
    ui.built = true;
    apply_layout(false);
    ui.jiggle = lv_timer_create(jiggle_cb, 40, nullptr);
    lv_timer_pause(ui.jiggle);
    build_settings(brightness, haptics, auto_rotate);
    if (hooks.decorate) {
        hooks.decorate(ui.status, ui.settings);
    }
    stagger(ui.pages[0], 0, 12);
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
    // White while connected, amber while connecting, red when it cannot.
    set_hidden(ui.vpn, status.vpn == LauncherVpn::Off);
    if (status.vpn != LauncherVpn::Off) {
        const uint32_t c = status.vpn == LauncherVpn::Connected ? TEXT : status.vpn == LauncherVpn::Connecting ? WARN : RED;
        set_color(ui.vpn_text, c);
        if (!lv_color_eq(lv_obj_get_style_border_color(ui.vpn, LV_PART_MAIN), lv_color_hex(c))) {
            lv_obj_set_style_border_color(ui.vpn, lv_color_hex(c), 0);
        }
    }
    update_status_bar();
    for (int i = 0; i < 2; ++i) {
        update_widget(i);
    }
    if (ui.badge) {
        if (glim.ready && glim.alerts > 0) {
            char v[8];
            snprintf(v, sizeof(v), "%d", std::min(glim.alerts, 99));
            set_text(ui.badge_text, v);
            set_hidden(ui.badge, false);
        } else {
            set_hidden(ui.badge, true);
        }
    }
    if (ui.swifty_dot) {
        set_hidden(ui.swifty_dot, !(swifty.listening || swifty.busy));
    }
    update_settings(status);
}

// Back home: editing ends (and saves), the icons settle in again, as a phone's do.
void launcher_ui_entered(ShellApp app)
{
    (void)app;
    lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
    close_picker();
    close_hidden();
    set_editing(false);
    stagger(ui.pages[size_t(std::min(ui.page, int(ui.pages.size()) - 1))], 0, 10);
}

void launcher_ui_show_page(int index)
{
    lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
    close_hidden();
    if (index >= 6) {
        open_settings();
        return;
    }
    const int page = std::clamp(index, 0, int(ui.pages.size()) - 1);
    lv_obj_scroll_to_x(ui.pager, page * PANEL, LV_ANIM_OFF);
    ui.page = page;
    page_dots();
}

bool launcher_ui_back()
{
    if (ui.picker) {
        close_picker();
    } else if (ui.hidden_sheet) {
        close_hidden();
    } else if (ui.editing) {
        set_editing(false);
    } else if (!lv_obj_has_flag(ui.settings, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
    } else if (ui.page > 0) {
        lv_obj_scroll_to_x(ui.pager, 0, LV_ANIM_ON);
        ui.page = 0;
        page_dots();
    } else {
        return false;
    }
    return true;
}

void launcher_ui_edit(bool on)
{
    set_editing(on);
}

void launcher_ui_hidden_apps(bool visible)
{
    if (visible) {
        open_hidden();
    } else {
        close_hidden();
    }
}

size_t launcher_ui_layout(char *out, size_t capacity)
{
    L::Layout copy = ui.layout;
    L::normalize(copy, order(), kind, keep, false);
    const std::string text = L::serialize(copy);
    return snprintf(out, capacity, "page %d of %d%s, layout %s", ui.page + 1, int(copy.pages.size()),
                    ui.editing ? ", editing" : "", text.c_str());
}

void launcher_ui_set_layout(const char *text)
{
    if (ui.drag) {
        return;
    }
    ui.layout = L::parse(text ? text : "");
    normalize();
    apply_layout(false);
    save_layout();
}

void launcher_ui_refilter()
{
    if (!ui.built || ui.drag) {
        return;
    }
    normalize();
    apply_layout(false);
}

void launcher_ui_calibration(bool visible, int radius)
{
    if (ui.calibration) {
        lv_obj_delete(ui.calibration);
        ui.calibration = ui.guide = ui.cal_value = nullptr;
    }
    if (!visible) {
        return;
    }
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

// ---- shortcut tiles ---------------------------------------------------------------------------
bool launcher_shortcut_set(const LauncherShortcut &s)
{
    if (!s.key || strncmp(s.key, "sc:", 3) || strlen(s.key) <= 3 || strlen(s.key) > L::MAX_KEY ||
            strpbrk(s.key, ",|# \n\r\t") || !s.run) {
        return false;
    }
    Shortcut v{s.key, s.name ? s.name : "", s.glyph ? s.glyph : "", s.accent, s.run, s.user};
    Shortcut *old = find_shortcut(v.key);
    if (old) {
        const bool same = old->name == v.name && old->glyph == v.glyph && old->accent == v.accent;
        *old = v;
        if (same || !ui.built || (ui.drag && ui.drag->key == v.key)) {
            return true;
        }
        delete_cell(v.key);  // a new look: drawn again in its place
    } else {
        ui.shortcuts.push_back(v);
    }
    if (ui.built) {
        Cell *c = make_cell(v.key);
        if (ui.drag) {  // mid-drag: placed on the drop
            return true;
        }
        normalize();
        apply_layout(false);
        (void)c;
    }
    return true;
}

void launcher_shortcut_remove(const char *key)
{
    if (!key) {
        return;
    }
    const std::string k = key;
    ui.shortcuts.erase(std::remove_if(ui.shortcuts.begin(), ui.shortcuts.end(), [&](const Shortcut & s) {
        return s.key == k;
    }), ui.shortcuts.end());
    L::remove(ui.layout, k);
    if (!ui.built) {
        return;
    }
    delete_cell(k);
    normalize();
    apply_layout(false);
    save_layout();
}

void launcher_shortcut_feedback(const char *key, int state)
{
    Cell *c = key && ui.built ? cell(key) : nullptr;
    if (!c || c->app) {
        return;
    }
    if (c->mark) {
        lv_obj_delete(c->mark);
    }
    if (c->mark_timer) {
        lv_timer_delete(c->mark_timer);
    }
    // A badge at the tile's lower corner: running, done or failed.
    lv_obj_t *m = box(c->obj, (PITCH + ICON) / 2 - 20, TOP + ICON - 20, 26, 26, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(m, lv_color_hex(state > 0 ? OK : state == 0 ? RED : FIELD), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m, 2, 0);
    lv_obj_set_style_border_color(m, lv_color_black(), 0);
    if (state < 0) {
        label(m, "\xE2\x80\xA6", 0, 1, 22, TEXT, 14, true, LV_TEXT_ALIGN_CENTER);
    } else {
        icon(m, state > 0 ? "check" : "x", 4, 4, 14, 0x101010, 3.0f);
    }
    c->mark = m;
    c->mark_timer = lv_timer_create(mark_clear, state < 0 ? 20000 : 1600, c);
    lv_timer_set_repeat_count(c->mark_timer, 1);
}
