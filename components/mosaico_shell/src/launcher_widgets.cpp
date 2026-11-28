// SPDX-License-Identifier: Apache-2.0
#include "launcher_widgets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "home_icons.h"
#include "mui.h"

namespace launcher_widgets {
namespace {
using namespace mui;

// Swifty's sky blue (also the weather's); the clock's graphite.
const Accent SKY = {0x7DF3FF, 0x2B5CF2, 0x35B2FF, 0x041428, 0x12305A, 0x060E1C};
const Accent GRAPHITE = {0xC9C9CF, 0x5A5A62, 0xB0B0B8, 0x101012, 0x2A2A2E, 0x151517};
const char *const DASH = "\xE2\x80\x94";

const Info INFO[KINDS] = {
    // name        title        the gallery's line                 glyph      app
    {"glimdock", "Glimdock", "CPU, memory and the homelab's health", "tech", "glimdock"},
    {"lights", "Lights", "How bright the lights are, and the TV", "bulb", "home"},
    {"music", "Music", "The speaker and its volume", "speaker", "home"},
    {"swifty", "Swifty", "What Swifty said last", "mic", "swifty"},
    {"battery", "Battery", "Charge, and whether it is charging", "power", nullptr},
    {"clock", "Clock", "The time and the date", "clock", nullptr},
    {"weather", "Weather", "Now, today's high and low, the next hours", nullptr, nullptr},
    {"calendar", "Date", "Today's date, large", "history", nullptr},
    {"reminder", "Reminder", "The next reminder Swifty set", "bell", nullptr},
    {"network", "Network", "Wi-Fi and the VPN", nullptr, nullptr},
};

// ---- weather glyphs -------------------------------------------------------------------
// Drawn on mui::icon's 24-unit grid from circles and lines, and kept as
// home_icons::Icon, so mui rasterises each once per size into a cached mask,
// as it does every other icon.
struct Circle {
    float x, y, r;
};

struct Shape {
    std::vector<std::vector<float>> points;
    std::vector<home_icons::Path> paths;
    home_icons::Icon icon{};

    void add(std::vector<float> &&xy)
    {
        if (xy.size() >= 4) {
            points.push_back(std::move(xy));
        }
    }
    void line(float x1, float y1, float x2, float y2)
    {
        add({x1, y1, x2, y2});
    }
    // The arcs of c whose points keep() keeps, each as a path.
    void arcs(const Circle &c, const std::function<bool(float, float)> &keep, int steps = 36)
    {
        auto at = [&](int i, float * x, float * y) {
            const float a = 6.2831853f * float(i) / float(steps);
            *x = c.x + c.r * cosf(a);
            *y = c.y + c.r * sinf(a);
        };
        // Start where the circle is cut, so no kept run wraps around.
        int start = 0;
        for (int i = 0; i < steps; ++i) {
            float x, y;
            at(i, &x, &y);
            if (!keep(x, y)) {
                start = i;
                break;
            }
        }
        std::vector<float> run;
        for (int k = 0; k <= steps; ++k) {
            float x, y;
            at(start + k, &x, &y);
            if (keep(x, y)) {
                run.push_back(x);
                run.push_back(y);
            } else {
                add(std::move(run));
                run.clear();
            }
        }
        add(std::move(run));
    }
    void finish()
    {
        paths.clear();
        for (const auto &p : points) {
            paths.push_back({p.data(), uint16_t(p.size() / 2)});
        }
        icon = {"weather", paths.data(), uint8_t(paths.size())};
    }
};

bool inside(const Circle &c, float x, float y, float margin = 0)
{
    const float dx = x - c.x, dy = y - c.y, r = c.r + margin;
    return dx * dx + dy * dy < r * r - 0.01f;
}

// A cloud: three bumps on a flat base, scaled by k and moved by (dx, dy).
struct Cloud {
    Circle c[3];
    float x1, x2, top, base;  // the base's box

    Cloud(float k, float dx, float dy)
        : c{{dx + 6.5f * k, dy + 15.5f * k, 3.5f * k}, {dx + 17.5f * k, dy + 15 * k, 4 * k}, {dx + 11.5f * k, dy + 11.5f * k, 5 * k}},
    x1(dx + 6.5f * k), x2(dx + 17.5f * k), top(dy + 15.2f * k), base(dy + 19 * k) {}

    bool contains(float x, float y, float m = 0) const
    {
        for (const Circle &o : c)
            if (inside(o, x, y, m)) {
                return true;
            }
        return x > x1 - m && x < x2 + m && y > top - m && y < base + m - 0.01f;
    }
    void draw(Shape &s) const
    {
        for (int i = 0; i < 3; ++i) {
            s.arcs(c[i], [&](float x, float y) {
                for (int j = 0; j < 3; ++j)
                    if (j != i && inside(c[j], x, y)) {
                        return false;
                    }
                return !(x > x1 + 0.01f && x < x2 - 0.01f && y > top && y < base + 0.2f) && y <= base + 0.05f;
            });
        }
        s.line(x1, base, x2, base);
    }
};

// A sun (rays outside r_in .. r_out), less what `hide` covers.
void sun(Shape &s, float x, float y, float r, float r_in, float r_out, const std::function<bool(float, float)> &hide)
{
    s.arcs({x, y, r}, [&](float px, float py) {
        return !hide(px, py);
    }, 28);
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * float(i) / 8, cx = cosf(a), cy = sinf(a);
        const float ax = x + r_in * cx, ay = y + r_in * cy, bx = x + r_out * cx, by = y + r_out * cy;
        if (!hide(ax, ay) && !hide(bx, by) && !hide((ax + bx) / 2, (ay + by) / 2)) {
            s.line(ax, ay, bx, by);
        }
    }
}

// A crescent moon: the circle o less the circle i, less what `hide` covers.
void moon(Shape &s, const Circle &o, const Circle &i, const std::function<bool(float, float)> &hide)
{
    s.arcs(o, [&](float x, float y) {
        return !inside(i, x, y) && !hide(x, y);
    });
    s.arcs(i, [&](float x, float y) {
        return inside(o, x, y) && !hide(x, y);
    });
}

enum GlyphShape { G_SUN, G_MOON, G_PARTLY_SUN, G_PARTLY_MOON, G_PARTLY_CLOUD, G_CLOUD, G_HIGH_CLOUD, G_FOG, G_RAIN,
                  G_DRIZZLE, G_SNOW, G_BOLT, G_SHAPES
                };
Shape *shapes()
{
    static Shape *s = nullptr;
    if (s) {
        return s;
    }
    s = new Shape[G_SHAPES];
    const auto none = [](float, float) {
        return false;
    };
    sun(s[G_SUN], 12, 12, 4.6f, 7.4f, 10.2f, none);
    moon(s[G_MOON], {12, 12.5f, 7.5f}, {16, 8.5f, 6.2f}, none);
    // Partly: a smaller sun or moon up and left, behind a cloud down and right
    // (with a little gap round the cloud).
    const Cloud partly(0.78f, 5.4f, 4.6f);
    const auto behind = [partly](float x, float y) {
        return partly.contains(x, y, 1.4f);
    };
    sun(s[G_PARTLY_SUN], 8.6f, 8.6f, 3.4f, 5.6f, 7.6f, behind);
    moon(s[G_PARTLY_MOON], {8.4f, 8.8f, 5}, {11.2f, 6.2f, 4.2f}, behind);
    partly.draw(s[G_PARTLY_CLOUD]);
    Cloud(1, 0, -1).draw(s[G_CLOUD]);
    // Rain, snow and storms fall from a cloud raised to make room.
    Cloud(0.86f, 1.7f, -3.2f).draw(s[G_HIGH_CLOUD]);
    s[G_FOG].line(5, 8.5f, 19, 8.5f);
    s[G_FOG].line(3, 12.5f, 21, 12.5f);
    s[G_FOG].line(6, 16.5f, 18, 16.5f);
    for (int i = 0; i < 3; ++i) {
        const float x = 8.4f + 4 * float(i);
        s[G_RAIN].line(x, 15.2f, x - 1.6f, 20.6f);
        s[G_DRIZZLE].line(x - 0.2f, 15.6f + (i == 1 ? 1.6f : 0), x - 0.9f, 18 + (i == 1 ? 1.6f : 0));
    }
    const float flakes[][2] = {{7.6f, 16.4f}, {12, 17.6f}, {16.4f, 16.4f}, {9.8f, 20.8f}, {14.2f, 20.8f}};
    for (const auto &f : flakes) {
        s[G_SNOW].line(f[0], f[1], f[0] + 0.01f, f[1] + 0.01f);  // a round-capped dot
    }
    s[G_BOLT].add({13.6f, 13.8f, 10.6f, 18.6f, 13.6f, 18.6f, 11, 23});
    for (int i = 0; i < G_SHAPES; ++i) {
        s[i].finish();
    }
    return s;
}

constexpr uint32_t SUN_INK = 0xFFD36B, MOON_INK = 0xE4E7FF, CLOUD_INK = 0xE2E5EC, RAIN_INK = 0x7DD3FF,
                   FOG_INK = 0xB9BEC8;

struct GlyphPart {
    int shape;
    uint32_t ink;
};

int glyph_parts(int code, bool day, GlyphPart out[2])
{
    switch (weather_sky(code)) {
    case WeatherSky::Clear:
        if (code == 1) {  // mostly clear: a wisp of cloud
            out[0] = {day ? G_PARTLY_SUN : G_PARTLY_MOON, day ? SUN_INK : MOON_INK};
            out[1] = {G_PARTLY_CLOUD, CLOUD_INK};
            return 2;
        }
        out[0] = {day ? G_SUN : G_MOON, day ? SUN_INK : MOON_INK};
        return 1;
    case WeatherSky::PartlyCloudy:
        out[0] = {day ? G_PARTLY_SUN : G_PARTLY_MOON, day ? SUN_INK : MOON_INK};
        out[1] = {G_PARTLY_CLOUD, CLOUD_INK};
        return 2;
    case WeatherSky::Cloudy: out[0] = {G_CLOUD, CLOUD_INK}; return 1;
    case WeatherSky::Fog: out[0] = {G_FOG, FOG_INK}; return 1;
    case WeatherSky::Drizzle:
        out[0] = {G_HIGH_CLOUD, CLOUD_INK};
        out[1] = {G_DRIZZLE, RAIN_INK};
        return 2;
    case WeatherSky::Rain:
        out[0] = {G_HIGH_CLOUD, CLOUD_INK};
        out[1] = {G_RAIN, RAIN_INK};
        return 2;
    case WeatherSky::Snow:
        out[0] = {G_HIGH_CLOUD, CLOUD_INK};
        out[1] = {G_SNOW, 0xFFFFFF};
        return 2;
    case WeatherSky::Storm:
        out[0] = {G_HIGH_CLOUD, CLOUD_INK};
        out[1] = {G_BOLT, SUN_INK};
        return 2;
    }
    return 0;
}

void draw_shape(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const auto *icon = static_cast<const home_icons::Icon *>(lv_event_get_user_data(e));
    draw_icon_mask(lv_event_get_layer(e), a, icon, lv_obj_get_style_line_color(o, LV_PART_MAIN),
                   lv_obj_get_style_line_width(o, LV_PART_MAIN));
}

// ---- formatting ---------------------------------------------------------------------------
// A temperature in the units set, rounded: "18°".
void degrees(char *out, size_t n, float celsius, bool fahrenheit)
{
    const float v = fahrenheit ? celsius * 9 / 5 + 32 : celsius;
    snprintf(out, n, "%d\xC2\xB0", int(lroundf(v)));
}

// "Sun 23 Nov" into "Sunday", "23" and "November".
void long_date(const char *date, char *weekday, size_t wn, char *day, size_t dn, char *month, size_t mn)
{
    static const char *const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const MONTHS[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                         "September", "October", "November", "December"
                                        };
    char wd[8] = "", mo[8] = "";
    int d = 0;
    *weekday = *day = *month = 0;
    if (sscanf(date, "%7s %d %7s", wd, &d, mo) != 3) {
        return;
    }
    strlcpy(weekday, wd, wn);
    for (const char *name : DAYS)
        if (!strncmp(name, wd, 3)) {
            strlcpy(weekday, name, wn);
        }
    snprintf(day, dn, "%d", d);
    strlcpy(month, mo, mn);
    for (const char *name : MONTHS)
        if (!strncmp(name, mo, 3)) {
            strlcpy(month, name, mn);
        }
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

void set_color(lv_obj_t *o, uint32_t color)
{
    if (o && !lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), lv_color_hex(color))) {
        lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    }
}
}  // namespace

// ---- the kinds --------------------------------------------------------------------------
const Info &info(int kind)
{
    return INFO[kind >= 0 && kind < KINDS ? kind : CLOCK];
}

int kind_of(const std::string &key)
{
    if (key.size() < 4 || key[0] != 'w' || key[2] != ':' || (key[1] != '2' && key[1] != '4')) {
        return -1;
    }
    const size_t end = key.find('~');
    const std::string name = key.substr(3, end == std::string::npos ? std::string::npos : end - 3);
    for (int k = 0; k < KINDS; ++k)
        if (name == INFO[k].name) {
            return k;
        }
    return -1;
}

std::string key_for(int kind, int width)
{
    return std::string(width >= 4 ? "w4:" : "w2:") + info(kind).name;
}

int tap_action(int kind)
{
    switch (kind) {
    case GLIMDOCK: return int(ShellApp::Glimdock);
    case LIGHTS:
    case MUSIC: return int(ShellApp::Home);
    case SWIFTY:
    case REMINDER: return int(ShellApp::Swifty);
    case NETWORK: return TAP_WIFI;
    case WEATHER: return TAP_REFRESH;
    default: return TAP_NONE;
    }
}

lv_obj_t *weather_glyph(lv_obj_t *parent, int x, int y, int size, int code, bool day)
{
    lv_obj_t *g = box(parent, x, y, size, size);
    GlyphPart parts[2];
    const int n = glyph_parts(code, day, parts);
    Shape *s = shapes();
    for (int i = 0; i < n; ++i) {
        lv_obj_t *o = box(g, 0, 0, size, size);
        lv_obj_set_style_line_color(o, lv_color_hex(parts[i].ink), 0);
        lv_obj_set_style_line_width(o, std::max(1, int(lroundf(1.9f * float(size) / 24.0f))), 0);
        lv_obj_add_event_cb(o, draw_shape, LV_EVENT_DRAW_MAIN, &s[parts[i].shape].icon);
    }
    return g;
}

lv_obj_t *kind_icon(lv_obj_t *parent, int kind, int x, int y, int size, uint32_t color)
{
    if (kind == WEATHER) {
        return weather_glyph(parent, x, y, size, 2, true);
    }
    if (kind == NETWORK) {  // Wi-Fi: LVGL's own symbol (mui's fonts fall back to it)
        lv_obj_t *b = box(parent, x, y, size, size);
        label(b, LV_SYMBOL_WIFI, 0, size / 12, size, color, size * 5 / 6, true, LV_TEXT_ALIGN_CENTER);
        return b;
    }
    return icon(parent, info(kind).glyph, x, y, size, color, 1.7f);
}

// ---- a widget ----------------------------------------------------------------------------
struct Widget {
    int kind = CLOCK, width = 2, w = 196;
    lv_obj_t *card = nullptr, *title = nullptr, *big = nullptr, *line1 = nullptr, *line2 = nullptr, *icon = nullptr,
              *spark = nullptr, *dot = nullptr, *corner = nullptr, *side1 = nullptr, *side2 = nullptr;
    // Weather: the condition now and the next hours.
    lv_obj_t *glyph = nullptr;
    int glyph_code = -1;
    bool glyph_day = true;
    struct Hour {
        lv_obj_t *time = nullptr, *glyph = nullptr, *temp = nullptr;
        int code = -1;
        bool day = true;
    } hours[5];
    // Glimdock, wide: up to three systems.
    lv_obj_t *item[3] = {}, *item_dot[3] = {};
};

namespace {
// Ink that stays readable over a card's fill.
void ink(Widget *w, bool dark)
{
    const uint32_t c = 0x2A1A05;
    if (w->icon) {
        set_line_color(w->icon, lv_color_hex(dark ? c : TEXT));
    }
    set_color(w->title, dark ? c : TEXT2);
    set_color(w->big, dark ? c : TEXT);
    set_color(w->line1, dark ? c : TEXT2);
    set_color(w->line2, dark ? c : TEXT3);
}

void set_glyph(lv_obj_t **slot, int *code_now, bool *day_now, lv_obj_t *parent, int x, int y, int size, int code, bool day)
{
    if (*slot && *code_now == code && *day_now == day) {
        return;
    }
    if (*slot) {
        lv_obj_delete(*slot);
    }
    *slot = weather_glyph(parent, x, y, size, code, day);
    *code_now = code;
    *day_now = day;
}

void update_weather(Widget *w, const Data &d)
{
    const WeatherReport &r = d.weather;
    char v[48];
    set_text(w->title, r.place[0] ? r.place : "Weather");
    if (!r.valid) {
        set_text(w->big, DASH);
        if (w->glyph) {
            lv_obj_delete(w->glyph);
            w->glyph = nullptr;
            w->glyph_code = -1;
        }
        const char *why = r.state == WeatherState::Locating ? "Finding where you are\xE2\x80\xA6"
                          : r.state == WeatherState::Fetching ? "Updating\xE2\x80\xA6"
                          : r.state == WeatherState::NoPlace  ? "Place not found"
                          : r.state == WeatherState::Failed   ? "No forecast yet"
                          : "Offline";
        set_text(w->line1, why);
        set_color(w->line1, r.state == WeatherState::NoPlace ? WARN : TEXT2);
        set_text(w->line2, r.state == WeatherState::NoPlace ? "Change it in Settings" : "");
        if (w->corner) {
            set_text(w->corner, "");
        }
        for (auto &h : w->hours) {
            if (h.time) {
                set_hidden(h.time, true);
                set_hidden(h.temp, true);
                if (h.glyph) {
                    set_hidden(h.glyph, true);
                }
            }
        }
        return;
    }
    degrees(v, sizeof(v), r.temp, r.fahrenheit);
    set_text(w->big, v);
    constexpr int GS = 48;  // the sky now, right of the temperature
    set_glyph(&w->glyph, &w->glyph_code, &w->glyph_day, w->card, 196 - 16 - GS, 30, GS, r.code, r.day);
    // Offline for a while: say since when, in amber, instead of the sky.
    if (r.stale) {
        snprintf(v, sizeof(v), "Offline \xC2\xB7 %s", r.updated);
        set_text(w->line1, v);
        set_color(w->line1, WARN);
    } else {
        set_text(w->line1, weather_text(r.code));
        set_color(w->line1, TEXT2);
    }
    char hi[12], lo[12];
    degrees(hi, sizeof(hi), r.high, r.fahrenheit);
    degrees(lo, sizeof(lo), r.low, r.fahrenheit);
    snprintf(v, sizeof(v), "H %s  L %s", hi, lo);
    set_text(w->line2, v);
    if (w->corner) {
        if (w->width >= 4) {
            snprintf(v, sizeof(v), r.stale ? "" : "Updated %s", r.updated);
        } else {
            strlcpy(v, r.stale ? "" : r.updated, sizeof(v));
        }
        set_text(w->corner, v);
    }
    for (int i = 0; i < 5; ++i) {
        Widget::Hour &h = w->hours[i];
        if (!h.time) {
            continue;
        }
        const bool have = i < r.hour_count;
        set_hidden(h.time, !have);
        set_hidden(h.temp, !have);
        if (!have) {
            if (h.glyph) {
                set_hidden(h.glyph, true);
            }
            continue;
        }
        const WeatherHour &f = r.hours[i];
        snprintf(v, sizeof(v), "%02d", f.hour);
        set_text(h.time, v);
        degrees(v, sizeof(v), float(f.temp10) / 10, r.fahrenheit);
        set_text(h.temp, v);
        const int x = 204 + i * 38;
        set_glyph(&h.glyph, &h.code, &h.day, w->card, x + 6, 40, 26, f.code, f.day);
        set_hidden(h.glyph, false);
    }
}

void update_reminder(Widget *w, const Data &d)
{
    if (!d.reminder) {
        set_text(w->big, DASH);
        set_text(w->line1, "No reminders");
        set_text(w->line2, "Ask Swifty to set one");
        return;
    }
    char v[40];
    struct tm at {
    }, now {};
    const time_t t = time(nullptr);
    localtime_r(&d.reminder_at, &at);
    localtime_r(&t, &now);
    snprintf(v, sizeof(v), "%02d:%02d", at.tm_hour, at.tm_min);
    set_text(w->big, v);
    set_text(w->line1, d.reminder_text);
    // Today, tomorrow, or the day.
    struct tm next = now;
    next.tm_mday += 1;
    next.tm_hour = 12;
    const time_t tomorrow = mktime(&next);
    struct tm tm_tomorrow {};
    localtime_r(&tomorrow, &tm_tomorrow);
    if (at.tm_year == now.tm_year && at.tm_yday == now.tm_yday) {
        strlcpy(v, "Today", sizeof(v));
    } else if (at.tm_year == tm_tomorrow.tm_year && at.tm_yday == tm_tomorrow.tm_yday) {
        strlcpy(v, "Tomorrow", sizeof(v));
    } else {
        strftime(v, sizeof(v), "%a %d %b", &at);
    }
    set_text(w->line2, v);
}
}  // namespace

Widget *create(lv_obj_t *parent, int kind, int width, int x, int y)
{
    auto *w = new Widget;
    w->kind = kind >= 0 && kind < KINDS ? kind : CLOCK;
    w->width = width >= 4 ? 4 : 2;
    w->w = card_width(w->width);
    kind = w->kind;
    const int W = w->w;
    const bool wide = w->width >= 4;
    const Accent &acc = kind == GLIMDOCK || kind == NETWORK ? CYAN : kind == SWIFTY || kind == WEATHER ? SKY
                        : kind == BATTERY ? GREEN : kind == CLOCK || kind == CALENDAR ? GRAPHITE
                        : kind == REMINDER ? AMBER : VIOLET;
    w->card = glass(parent, x, y, W, HEIGHT, 26, acc);
    if (kind == LIGHTS || kind == MUSIC || kind == BATTERY) {
        card_fill(w->card, 0, 0xFFD9A0, 0xF3B56A);
    }
    // The title row: an icon (Glimdock: its status dot) and a name.
    if (kind == GLIMDOCK) {
        w->dot = dot(w->card, 16, 18, 9, OFF, false);
        w->title = label(w->card, "glimdock", 32, 12, 150, TEXT2, 14, true);
    } else if (kind == WEATHER) {
        w->icon = icon(w->card, "pin", 14, 12, 20, TEXT2, 1.8f);
        w->title = label(w->card, "Weather", 38, 13, wide ? 150 : 100, TEXT2, 14, true);
        w->corner = wide ? label(w->card, "", 204, 108, 182, TEXT3, 12, true, LV_TEXT_ALIGN_RIGHT)
                    : label(w->card, "", W - 66, 14, 50, TEXT3, 12, true, LV_TEXT_ALIGN_RIGHT);
    } else if (kind == CALENDAR) {
        w->title = label(w->card, "", 16, 13, W - 32, TEXT2, 14, true);
    } else if (kind == NETWORK) {
        w->title = label(w->card, LV_SYMBOL_WIFI "  Network", 16, 13, W - 32, TEXT2, 14, true);
    } else {
        w->icon = icon(w->card, INFO[kind].glyph, 14, 12, 22, TEXT, 1.6f);
        w->title = label(w->card, INFO[kind].title, 42, 13, 140, TEXT2, 14, true);
    }
    // The body.
    if (kind == SWIFTY) {
        w->line1 = label(w->card, "", 16, 42, W - 30, TEXT, 15, false);
        lv_label_set_long_mode(w->line1, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(w->line1, 82);
    } else if (kind == CALENDAR) {
        w->big = label(w->card, "", 14, 26, wide ? 110 : W - 26, TEXT, 58, true);
        w->line1 = label(w->card, "", 16, 102, wide ? 180 : W - 26, TEXT2, 14, true);
        if (wide) {  // and the time, large, beside it
            w->side1 = label(w->card, "", 200, 30, 184, TEXT, 44, true, LV_TEXT_ALIGN_RIGHT);
            w->side2 = label(w->card, "", 200, 90, 184, TEXT3, 14, true, LV_TEXT_ALIGN_RIGHT);
        }
    } else if (kind == CLOCK && wide) {
        w->big = label(w->card, "\xE2\x80\x94", 16, 34, 200, TEXT, 56, true);
        w->side1 = label(w->card, "", 212, 40, 172, TEXT, 22, true, LV_TEXT_ALIGN_RIGHT);
        w->side2 = label(w->card, "", 212, 74, 172, TEXT2, 16, true, LV_TEXT_ALIGN_RIGHT);
        w->line1 = label(w->card, "", 16, 108, 368, TEXT3, 13, true);
    } else {
        const int text_w = kind == WEATHER ? (wide ? 196 : W) - 26 : W - 26;
        w->big = label(w->card, DASH, 16, 34, kind == WEATHER ? 110 : text_w, TEXT, 34, true);
        w->line1 = label(w->card, "", 16, 78, text_w, TEXT2, 13, true);
        w->line2 = label(w->card, "", 16, 104, text_w, TEXT3, 13, true);
    }
    if (kind == GLIMDOCK) {
        lv_obj_add_flag(w->line2, LV_OBJ_FLAG_HIDDEN);
        w->spark = spark(w->card, 1, 96, W - 2, 38, CYAN.glow);
        if (wide) {  // the systems, at the right
            w->side1 = label(w->card, "", 212, 13, 172, TEXT2, 14, true, LV_TEXT_ALIGN_RIGHT);
            for (int i = 0; i < 2; ++i) {
                w->item_dot[i] = dot(w->card, 222, 44 + i * 24, 8, OFF, false);
                w->item[i] = label(w->card, "", 236, 38 + i * 24, 148, TEXT, 13, true);
            }
        }
    }
    if (kind == WEATHER && wide) {  // the next hours, at the right
        for (int i = 0; i < 5; ++i) {
            const int hx = 204 + i * 38;
            w->hours[i].time = label(w->card, "", hx, 16, 38, TEXT3, 13, true, LV_TEXT_ALIGN_CENTER);
            w->hours[i].temp = label(w->card, "", hx, 74, 38, TEXT, 15, true, LV_TEXT_ALIGN_CENTER);
        }
    }
    return w;
}

lv_obj_t *card(Widget *w)
{
    return w->card;
}

int kind(const Widget *w)
{
    return w->kind;
}

int width(const Widget *w)
{
    return w->width;
}

void destroy(Widget *w)
{
    if (!w) {
        return;
    }
    if (w->card) {
        lv_obj_delete(w->card);
    }
    delete w;
}

void update(Widget *w, const Data &d)
{
    char v[64];
    switch (w->kind) {
    case GLIMDOCK: {
        const glimdock_summary_t &g = d.glim;
        if (isfinite(g.cpu)) {
            snprintf(v, sizeof(v), "%d%% CPU", int(lroundf(g.cpu)));
        } else {
            strlcpy(v, g.started ? DASH : "Homelab", sizeof(v));
        }
        set_text(w->big, v);
        if (isfinite(g.memory)) {
            snprintf(v, sizeof(v), "Memory %d%%", int(lroundf(g.memory)));
        } else {
            strlcpy(v, g.status, sizeof(v));
        }
        set_text(w->line1, v);
        set_bg_color(w->dot, lv_color_hex(!g.ready ? OFF : g.alerts ? RED : OK));
        float top = 40;
        for (int i = 0; i < g.history_count; ++i) {
            top = std::max(top, g.history[i] * 1.25f);
        }
        spark_set(w->spark, g.history, g.history_count, std::min(100.0f, top));
        if (w->side1) {
            if (!g.ready) {
                v[0] = 0;
            } else if (g.nodes > 0) {
                snprintf(v, sizeof(v), "%d of %d healthy", g.healthy, g.nodes);
            } else {
                strlcpy(v, g.alerts ? "Alerts" : "Healthy", sizeof(v));
            }
            if (g.ready && g.alerts > 0) {
                const size_t n = strlen(v);
                snprintf(v + n, sizeof(v) - n, "%s%d alert%s", n ? " \xC2\xB7 " : "", g.alerts, g.alerts == 1 ? "" : "s");
            }
            set_text(w->side1, v);
            set_color(w->side1, g.ready && g.alerts ? RED : TEXT2);
            for (int i = 0; i < 2; ++i) {
                const bool have = g.ready && i < g.item_count;
                set_hidden(w->item[i], !have);
                set_hidden(w->item_dot[i], !have);
                if (have) {
                    const auto &it = g.items[i];
                    if (it.note[0]) {
                        snprintf(v, sizeof(v), "%s  %s", it.name, it.note);
                    } else {
                        strlcpy(v, it.name, sizeof(v));
                    }
                    set_text(w->item[i], v);
                    set_bg_color(w->item_dot[i], lv_color_hex(it.state == 0 ? OK : it.state == 1 ? WARN : RED));
                }
            }
        }
        break;
    }
    case LIGHTS: {
        const home_summary_t &h = d.home;
        const bool warm = !h.lights || (h.lights_hue >= 20 && h.lights_hue <= 50);
        const int lp = h.lights && h.lights_on ? h.lights_pct : 0;
        card_fill_set(w->card, lp, warm ? 0xFFD9A0 : hsl(float(h.lights_hue), 0.85f, 0.76f),
                      warm ? 0xF3B56A : hsl(float(h.lights_hue), 0.85f, 0.62f));
        ink(w, lp > 45);
        if (!h.paired) {
            strlcpy(v, h.started ? "Pair" : "Home", sizeof(v));
        } else if (h.lights && h.lights_on) {
            snprintf(v, sizeof(v), "%d%%", h.lights_pct);
        } else {
            strlcpy(v, h.lights ? "Off" : DASH, sizeof(v));
        }
        set_text(w->big, v);
        set_text(w->line1, h.paired ? (h.lights ? h.lights_state : "No lights") : "Open Home to pair");
        set_text(w->line2, h.tv ? h.tv_name : "");
        break;
    }
    case MUSIC: {
        const home_summary_t &h = d.home;
        const int vol = h.music && h.volume > 0 ? h.volume : 0;
        card_fill_set(w->card, vol, VIOLET.glow, VIOLET.glow);
        ink(w, false);
        if (h.music && h.volume >= 0) {
            snprintf(v, sizeof(v), "%d", h.volume);
        } else {
            strlcpy(v, DASH, sizeof(v));
        }
        set_text(w->big, v);
        set_text(w->line1, h.music ? h.music_state : h.paired ? "No speaker" : "Open Home to pair");
        set_text(w->line2, h.music ? "Volume" : "");
        break;
    }
    case SWIFTY: {
        std::string plain;
        for (const char *c = d.swifty.last; *c; ++c)
            if (*c != '*' && *c != '`' && *c != '#') {
                plain += *c == '\n' ? ' ' : *c;
            }
        set_text(w->line1, plain.empty() ? (d.swifty.configured ? "Hold the AI key and ask me anything."
                                            : "Set me up in Settings, Web settings.")
                 : plain.c_str());
        set_text(w->title, d.swifty.listening ? "Listening\xE2\x80\xA6" : d.swifty.busy ? "Thinking\xE2\x80\xA6" : "Swifty");
        break;
    }
    case BATTERY: {
        const int b = d.battery;
        card_fill_set(w->card, b > 0 ? b : 0, b >= 0 && b <= 20 ? RED : GREEN.glow, b >= 0 && b <= 20 ? RED : GREEN.glow);
        ink(w, b > 45);
        if (b >= 0) {
            snprintf(v, sizeof(v), "%d%%", b);
        } else {
            strlcpy(v, DASH, sizeof(v));
        }
        set_text(w->big, v);
        set_text(w->line1, b < 0 ? "No reading" : d.charging ? "Charging" : "On battery");
        set_text(w->line2, "");
        break;
    }
    case WEATHER:
        update_weather(w, d);
        break;
    case CALENDAR: {
        char weekday[16], day[8], month[16];
        long_date(d.date, weekday, sizeof(weekday), day, sizeof(day), month, sizeof(month));
        set_text(w->title, weekday[0] ? weekday : "Date");
        set_text(w->big, day[0] ? day : DASH);
        set_text(w->line1, month);
        if (w->side1) {
            set_text(w->side1, d.clock);
            set_text(w->side2, d.reminder ? d.reminder_text : "");
        }
        break;
    }
    case REMINDER:
        update_reminder(w, d);
        break;
    case NETWORK: {
        const bool on = d.wifi_connected;
        set_text(w->big, on ? "Online" : d.wifi_configured ? "Joining\xE2\x80\xA6" : "Offline");
        set_color(w->big, on ? TEXT : d.wifi_configured ? WARN : RED);
        set_text(w->line1, d.ssid[0] ? d.ssid : "No network set");
        const char *vpn = d.vpn == LauncherVpn::Connected ? "VPN connected" : d.vpn == LauncherVpn::Connecting
                          ? "VPN connecting\xE2\x80\xA6" : d.vpn == LauncherVpn::Trouble ? "VPN can't connect" : nullptr;
        if (vpn) {
            set_text(w->line2, vpn);
        } else {
            set_text(w->line2, on ? d.ip : "");
        }
        set_color(w->line2, d.vpn == LauncherVpn::Trouble ? RED : d.vpn == LauncherVpn::Connecting ? WARN : TEXT3);
        break;
    }
    default: {  // clock
        set_text(w->big, d.clock);
        if (w->side1) {
            char weekday[16], day[8], month[16];
            long_date(d.date, weekday, sizeof(weekday), day, sizeof(day), month, sizeof(month));
            set_text(w->side1, weekday);
            snprintf(v, sizeof(v), "%s %s", day, month);
            set_text(w->side2, day[0] ? v : "");
            if (d.reminder) {
                struct tm at {};
                localtime_r(&d.reminder_at, &at);
                snprintf(v, sizeof(v), "Next: %02d:%02d %.40s", at.tm_hour, at.tm_min, d.reminder_text);
                set_text(w->line1, v);
            } else {
                set_text(w->line1, "");
            }
        } else {
            set_text(w->line1, d.date);
            set_text(w->line2, "");
        }
        break;
    }
    }
}

}  // namespace launcher_widgets
