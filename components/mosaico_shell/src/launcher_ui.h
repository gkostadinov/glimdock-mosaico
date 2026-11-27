// SPDX-License-Identifier: Apache-2.0
// Launcher: one full-screen page per application (swipe between them), plus
// Settings, and the corner calibration overlay. Pure LVGL, shared by the
// device shell and the host preview.
#pragma once

#include "glimdock_app.h"
#include "camera_app.h"
#include "files_app.h"
#include "home_app.h"
#include "swifty_app.h"
#include "shell_internal.h"

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
};

// portrait: a still of Swifty for its page (may be null).
// widgets: the two widget kinds (see launcher_ui.cpp), -1 for the defaults.
lv_obj_t *launcher_ui_build(const LauncherHooks &hooks, int radius, int brightness, bool haptics, bool auto_rotate,
                            const lv_image_dsc_t *portrait, int left_widget = -1, int right_widget = -1);
void launcher_ui_update(const LauncherStatus &status, const glimdock_summary_t &glim, const home_summary_t &home,
                        const camera_summary_t &camera, const swifty_summary_t &swifty, const files_summary_t &files);
// The launcher was just shown: settle on the page of app (Launcher keeps the
// current page) and play its entrance.
void launcher_ui_entered(ShellApp app);
void launcher_ui_calibration(bool visible, int radius);
// Jump to a page (0 Glimdock, 1 Home, 2 Swifty, 3 Camera, 4 Files, 5 Tools, 6 Settings) with its entrance.
void launcher_ui_show_page(int index);
