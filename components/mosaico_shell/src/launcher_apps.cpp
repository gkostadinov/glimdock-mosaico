// SPDX-License-Identifier: Apache-2.0
// The home screen's apps. To add an app: its ShellApp (shell_internal.h), its
// screen in shell.cpp's shell_show, and one line here.
#include "launcher_apps.h"
#include "vpn_app.h"

#include <string.h>

namespace {
using namespace mui;
// Swifty's sky blue, from the bird's own colours; Files' teal; Tools' coral;
// the webcam's rose; the calculator's graphite; Games' neon plum.
const Accent SKY = {0x7DF3FF, 0x2B5CF2, 0x35B2FF, 0x041428, 0x12305A, 0x060E1C};
const Accent TEAL = {0x8CF5DC, 0x139E86, 0x3DDCBB, 0x022019, 0x0C2E28, 0x061613};
const Accent CORAL = {0xFFB199, 0xE0533D, 0xFF8A70, 0x2A0A04, 0x3A1610, 0x1A0906};
const Accent ROSE = {0xFFA3B5, 0xC8325A, 0xF0577A, 0x2A0610, 0x3A1420, 0x1A080D};
const Accent SLATE = {0x8A8A92, 0x3A3A40, 0x4A4A52, 0xFFFFFF, 0x2A2A2E, 0x151517};
const Accent PLUM = {0xFFA8F0, 0xB43CFF, 0xD452FF, 0x1E0630, 0x30144A, 0x150824};
}  // namespace

const LauncherApp LAUNCHER_APPS[] = {
    // key            name           glyph      accent    opens
    {"glimdock", "Glimdock", nullptr, &VIOLET, int(ShellApp::Glimdock), false},
    {"home", "Home", "house", &VIOLET, int(ShellApp::Home), false},
    {"swifty", "Swifty", "mic", &SKY, int(ShellApp::Swifty), false},
    {"camera", "Camera", "camera", &AMBER, int(ShellApp::Camera), false},
    {"gallery", "Gallery", "photos", &AMBER, int(ShellApp::Gallery), false},
    {"files", "Files", "folder", &TEAL, int(ShellApp::Files), false},
    {"tools", "Tools", "torch", &CORAL, int(ShellApp::Tools), false},
    {"settings", "Settings", "sliders", &GREEN, LAUNCHER_OPEN_SETTINGS, true},
    // The second page
    {"webcam", "Webcam", "webcam", &ROSE, int(ShellApp::Webcam), false},
    {"interaction", "Interaction", "remote", &CYAN, int(ShellApp::Interaction), false},
    {"calculator", "Calculator", "calc", &SLATE, int(ShellApp::Calculator), false},
    {"vpn", "VPN", "lock", &vpn_accent, int(ShellApp::Vpn), false},
    {"games", "Games", "gamepad", &PLUM, int(ShellApp::Games), false},
};
const int LAUNCHER_APP_COUNT = int(sizeof(LAUNCHER_APPS) / sizeof(LAUNCHER_APPS[0]));

const LauncherApp *launcher_app(const char *key)
{
    for (const LauncherApp &a : LAUNCHER_APPS)
        if (!strcmp(a.key, key)) {
            return &a;
        }
    return nullptr;
}

const char *launcher_app_key(ShellApp app)
{
    for (const LauncherApp &a : LAUNCHER_APPS)
        if (a.open == int(app)) {
            return a.key;
        }
    return nullptr;
}
