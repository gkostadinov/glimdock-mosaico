// SPDX-License-Identifier: Apache-2.0
// Launcher: the home screen (pages of app icons and shortcut tiles, two
// widgets on the first page, editable like a phone's), Settings, and the corner
// calibration overlay. Pure LVGL, shared by the device shell and the host
// preview. Every call runs on the LVGL task (or with its lock held).
#pragma once

#include <stddef.h>

#include "glimdock_app.h"
#include "camera_app.h"
#include "files_app.h"
#include "home_app.h"
#include "mui.h"
#include "swifty_app.h"
#include "shell_internal.h"

// The VPN's badge in the status bar: none while off.
enum class LauncherVpn : uint8_t { Off, Connected, Connecting, Trouble };

struct LauncherStatus {
    const char *clock;  // "HH:MM" or "--:--"
    const char *date;   // "Sun 23 Nov"
    bool wifi_connected, wifi_configured;
    const char *ssid, *ip, *versions;
    int battery_pct;  // -1 unknown
    bool charging;
    const char *zone;  // the time zone as Settings shows it, e.g. "Automatic · London"
    bool camera_auto;  // the camera finds front/rear by itself
    // Web settings (web_settings.h): on, where, the code to enter there, and
    // why it is off (empty: no reason to give).
    bool web_on = false;
    const char *web_url = "", *web_code = "", *web_note = "";
    // The always-on display's timeout in seconds (one of ALWAYS_ON_CHOICES, 0
    // off); -1 unknown.
    int always_on = -1;
    // The VPN, as a badge after the Wi-Fi icon.
    LauncherVpn vpn = LauncherVpn::Off;
};

struct LauncherHooks {
    void (*open)(ShellApp app);
    void (*save_radius)(int radius);           // persists, then restarts
    void (*brightness)(int percent, bool save);  // live while dragging; save on release
    void (*haptics)(bool enabled);
    void (*auto_rotate)(bool enabled);
    void (*save_widgets)(int left, int right);  // the home screen's two widget kinds
    void (*time_zone)(int step);                // Settings: the next (+1) or previous (-1) time zone choice
    void (*camera_auto)(bool enabled);          // Settings: the camera's automatic front/rear
    void (*web_settings)(bool on);              // Settings: the web settings server
    void (*always_on)(int seconds);             // Settings: the always-on display's timeout (0 off)
    // The home screen's layout changed (a drop, a hide, a reset): persist this
    // text and pass it to launcher_ui_build next time. Called on the LVGL task.
    void (*save_layout)(const char *layout);
    void (*tick)();  // a haptic tick: editing starts, a page turns under a dragged icon
    // The shortcuts' editor (optional): a Shortcuts button on the hidden apps'
    // sheet, which the + opens while editing.
    void (*shortcuts)();
};

// portrait: a still of Swifty (unused; may be null).
// widgets: the two widget kinds (see launcher_ui.cpp), -1 for the defaults.
// layout: the text save_layout last gave (null or "" for the default layout).
lv_obj_t *launcher_ui_build(const LauncherHooks &hooks, int radius, int brightness, bool haptics, bool auto_rotate,
                            const lv_image_dsc_t *portrait, int left_widget = -1, int right_widget = -1,
                            const char *layout = nullptr);
void launcher_ui_update(const LauncherStatus &status, const glimdock_summary_t &glim, const home_summary_t &home,
                        const camera_summary_t &camera, const swifty_summary_t &swifty, const files_summary_t &files);
// The launcher was just shown: editing ends (saved), overlays close, and the
// page in view plays its entrance.
void launcher_ui_entered(ShellApp app);
void launcher_ui_calibration(bool visible, int radius);
// Shows a home page (0 the first) at once, or Settings for 6 and up.
void launcher_ui_show_page(int index);
// Editing the home screen: jiggling icons, drag to move, − to hide, Done.
void launcher_ui_edit(bool on);
// The hidden apps' sheet (also opened from Settings and while editing).
void launcher_ui_hidden_apps(bool visible);
// "page 1 of 2, layout glimdock,home,...|webcam,...#hidden" for diagnostics (RPC 53).
size_t launcher_ui_layout(char *out, size_t capacity);
// Replaces the layout (launcher_layout.h's text form; "" the default) and saves it.
void launcher_ui_set_layout(const char *layout);

// ---- Shortcut tiles ------------------------------------------------------------------
// A tile on the home screen that runs an action instead of opening an app. The
// launcher keeps its place and whether it is hidden (with the layout, in NVS);
// whoever registers it keeps what it does. Register at every start, before or
// after launcher_ui_build: a known key comes back in its saved place, a new one
// goes at the end of the last page.
struct LauncherShortcut {
    const char *key;            // "sc:" then a stable id, up to 23 characters, no ',' '|' '#' or spaces (copied)
    const char *name;           // the label under the tile (copied)
    const char *glyph;          // a mui::icon name, e.g. "bulb", "tv", "play" (copied)
    const mui::Accent *accent;  // the glyph's colour (nullptr: violet); kept, so pass a static
    // On a tap, on the LVGL task: keep it short, hand slow work to another task.
    void (*run)(const char *key, void *user);
    void *user;
};
// Adds the tile, or updates the one with this key. False for a bad key or no run.
bool launcher_shortcut_set(const LauncherShortcut &shortcut);
// Removes the tile for good (and forgets its place).
void launcher_shortcut_remove(const char *key);
// A badge on the tile for how its run went: 1 done (a check), 0 failed (a
// cross), -1 running (until the next call, at most 20 s). Done and failed
// clear after 1.6 s.
void launcher_shortcut_feedback(const char *key, int state);
