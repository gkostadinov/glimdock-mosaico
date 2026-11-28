// SPDX-License-Identifier: Apache-2.0
// The home screen's widgets: what each kind shows, as a card 2 cells wide
// (196 x 136, the size of the first page's two widgets before they could move)
// or 4 wide (400 x 136). The launcher places them (launcher_layout.h: a
// widget's key is "w2:" or "w4:" then the kind's name); this draws them from
// the data it passes. Pure LVGL, shared by the device and the host preview;
// every call on the LVGL task.
#pragma once

#include <string>

#include "launcher_ui.h"
#include "weather.h"

namespace launcher_widgets {

// The first six are the old picker's, in its order: the numbers firmware 1.2
// saved for the two fixed widgets (NVS w0, w1), which migration reads.
enum Kind { GLIMDOCK, LIGHTS, MUSIC, SWIFTY, BATTERY, CLOCK, WEATHER, CALENDAR, REMINDER, NETWORK, KINDS };

struct Info {
    const char *name;   // in its key: "weather" (stable: saved in the layout)
    const char *title;  // "Weather"
    const char *blurb;  // the gallery's line about it
    const char *glyph;  // a mui::icon name
    // The app it shows (its widget hides with the app: launcher_ui_set_app_filter),
    // or nullptr.
    const char *app;
};
const Info &info(int kind);
// The kind a key names ("w2:weather~1": WEATHER), -1 when it is not a widget
// this firmware knows.
int kind_of(const std::string &key);
// The key for a kind at a width (2 or 4), without the "~n" of a second one.
std::string key_for(int kind, int width);

constexpr int HEIGHT = 136;
inline int card_width(int width)
{
    return width >= 4 ? 400 : 196;
}

// What widgets show, gathered once per update.
struct Data {
    char clock[8] = "--:--", date[24] = "";
    int battery = -1;
    bool charging = false;
    bool wifi_connected = false, wifi_configured = false;
    char ssid[40] = "", ip[24] = "";
    LauncherVpn vpn = LauncherVpn::Off;
    glimdock_summary_t glim{};
    home_summary_t home{};
    swifty_summary_t swifty{};
    WeatherReport weather{};
    bool reminder = false;  // one is set
    time_t reminder_at = 0;
    char reminder_text[96] = "";
};

struct Widget;
// A card of this kind and width at (x, y) in parent; nothing is clickable yet
// (the launcher adds its own events to card()).
Widget *create(lv_obj_t *parent, int kind, int width, int x, int y);
lv_obj_t *card(Widget *w);
int kind(const Widget *w);
int width(const Widget *w);
void update(Widget *w, const Data &data);
void destroy(Widget *w);  // the card too

// What a tap does when not editing: an app to open (ShellApp), or one of these.
constexpr int TAP_NONE = -1, TAP_WIFI = -2, TAP_REFRESH = -3;
int tap_action(int kind);

// A weather condition's drawing (WMO code, day or night) at (x, y), size px.
lv_obj_t *weather_glyph(lv_obj_t *parent, int x, int y, int size, int code, bool day);
// A kind's icon, as the gallery shows it.
lv_obj_t *kind_icon(lv_obj_t *parent, int kind, int x, int y, int size, uint32_t color);

}  // namespace launcher_widgets
