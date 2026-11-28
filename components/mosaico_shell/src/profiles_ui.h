// SPDX-License-Identifier: Apache-2.0
// The profiles' screens (profiles.h), over any app on the top layer, laid
// out for the round panel: the lock screen that picks a profile, the face
// scan and the round PIN pad, the profile's badge (status bar, Settings) and
// Settings > Profiles with its editor. Pure LVGL over profiles.h and
// face_id.h, so the desktop preview draws them. LVGL task.
#pragma once

#include <stdint.h>

#include "lvgl.h"

// An app as the editor lists it.
struct ProfilesUiApp {
    int slot;           // its ShellApp value, or PROFILE_APP_SETTINGS
    const char *name;   // "Home"
    const char *glyph;  // a mui::icon name; nullptr: the Glimdock mark's letter
    bool read_only;     // the app has a read-only mode, so the editor offers it
};

struct ProfilesUiHooks {
    const ProfilesUiApp *apps = nullptr;
    int app_count = 0;
    // A PIN's hash takes ~0.5 s: work runs off the LVGL task, then done back
    // on it (both get arg). Unset: both run at once, here.
    void (*background)(void (*work)(void *), void (*done)(void *), void *arg) = nullptr;
    void (*haptic)(bool strong) = nullptr;
};

void profiles_ui_init(const ProfilesUiHooks &hooks);
// The lock screen: who's using it (tap one to sign in), and Lock.
void profiles_ui_switcher();
// Straight to signing into one profile (its face, then its PIN).
void profiles_ui_sign_in(uint8_t id);
// Settings > Profiles: set up, the list, the editor.
void profiles_ui_settings();
void profiles_ui_close();
bool profiles_ui_showing();
// The AI key: one page back; false when nothing is open.
bool profiles_ui_back();
// The signed-in profile's round badge at (x, y), size across; a tap opens the
// switcher. It follows changes by itself and hides while profiles are off.
// settings: Settings' entry to Settings > Profiles instead, shown always (a
// person while profiles are off, to set them up).
lv_obj_t *profiles_ui_badge(lv_obj_t *parent, int x, int y, int size, bool settings = false);
// A short note in a pill at the foot of the panel ("Not available for this profile").
void profiles_ui_note(const char *text);
// A small lasting label at the foot of an app's screen, for read-only apps
// ("Read-only for this profile"); hidden until shown with mui::set_hidden.
lv_obj_t *profiles_ui_ribbon(lv_obj_t *parent, const char *text);
// Redraws badges and an open page after a change from elsewhere (an RPC, the auto-lock).
void profiles_ui_refresh();
