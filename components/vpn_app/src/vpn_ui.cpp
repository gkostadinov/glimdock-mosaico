// SPDX-License-Identifier: Apache-2.0
// The VPN's screen: a big switch, what the tunnel is doing in words, its
// address, handshake and traffic, and whether it connects at start-up.
#include <stdio.h>
#include <string.h>

#include "mui.h"
#include "vpn_ui.h"

const mui::Accent vpn_accent = {0xB4BEFF, 0x4F46E5, 0x8C93FF, 0x0B0A2A, 0x1A1846, 0x0C0B22};

namespace {
using namespace mui;
const Accent &INDIGO = vpn_accent;

struct {
    vpn_hooks_t hooks{};
    bool built = false, active = false;
    lv_obj_t *screen = nullptr, *dot = nullptr, *power = nullptr, *state = nullptr, *detail = nullptr, *card = nullptr;
    lv_obj_t *address = nullptr, *handshake = nullptr, *rx = nullptr, *tx = nullptr, *auto_toggle = nullptr;
    lv_timer_t *timer = nullptr;
    bool shown_on = false;
} U;

void bytes_text(uint64_t n, char *out, size_t cap)
{
    if (n < 1024) {
        snprintf(out, cap, "%u B", unsigned(n));
    } else if (n < 1024 * 1024) {
        snprintf(out, cap, "%.1f KB", double(n) / 1024);
    } else if (n < 1024ull * 1024 * 1024) {
        snprintf(out, cap, "%.1f MB", double(n) / (1024 * 1024));
    } else {
        snprintf(out, cap, "%.2f GB", double(n) / (1024.0 * 1024 * 1024));
    }
}

void age_text(int32_t s, char *out, size_t cap)
{
    if (s < 0) {
        strlcpy(out, "None yet", cap);
    } else if (s < 5) {
        strlcpy(out, "Just now", cap);
    } else if (s < 120) {
        snprintf(out, cap, "%ld s ago", long(s));
    } else if (s < 2 * 3600) {
        snprintf(out, cap, "%ld min ago", long(s / 60));
    } else {
        snprintf(out, cap, "%ld h ago", long(s / 3600));
    }
}

// The server's name for the screen: the endpoint without its port.
void server_name(const char *endpoint, char *out, size_t cap)
{
    strlcpy(out, endpoint, cap);
    char *colon = strrchr(out, ':');
    if (colon && out[0] != '[') {
        *colon = 0;
    }
}

void update()
{
    vpn_summary_t s;
    vpn_app_summary(&s);
    char server[72];
    server_name(s.endpoint, server, sizeof(server));
    if (s.on != U.shown_on) {
        toggle_set(U.power, s.on, true);
        U.shown_on = s.on;
    }
    const char *word = "Off";
    uint32_t color = TEXT3;
    char detail[200] = "";
    switch (s.state) {
    case VPN_CONNECTED:
        word = "Connected", color = OK;
        snprintf(detail, sizeof(detail), "%s goes through\n%s", s.full_tunnel ? "All internet traffic" : "The tunnel's network",
                 server);
        break;
    case VPN_CONNECTING:
        word = "Connecting\xE2\x80\xA6", color = WARN;
        snprintf(detail, sizeof(detail), "Shaking hands with\n%s", server);
        break;
    case VPN_WAITING:
        word = "Waiting for Wi-Fi", color = WARN;
        strlcpy(detail, "The VPN connects once Wi-Fi is back. Until then nothing goes around it.", sizeof(detail));
        break;
    case VPN_NOT_SET_UP:
        word = "Not set up", color = RED;
        strlcpy(detail, s.error, sizeof(detail));
        break;
    case VPN_ERROR:
        word = "Trouble connecting", color = RED;
        strlcpy(detail, s.error, sizeof(detail));
        break;
    default:
        if (!s.configured) {
            word = "Not set up";
            strlcpy(detail, "Add a WireGuard configuration in Web settings (Settings on the launcher), then switch on.",
                    sizeof(detail));
        } else {
            snprintf(detail, sizeof(detail), "Switch on to send %s\nthrough %s",
                     s.full_tunnel ? "all internet traffic" : "the tunnel's network", server);
        }
        break;
    }
    set_text(U.state, word);
    set_text_color(U.state, lv_color_hex(color));
    set_bg_color(U.dot, lv_color_hex(color));
    set_text(U.detail, detail);
    char b[32];
    set_text(U.address, s.address[0] ? s.address : "\xE2\x80\x94");
    age_text(s.handshake_age_s, b, sizeof(b));
    set_text(U.handshake, s.on ? b : "\xE2\x80\x94");
    bytes_text(s.rx_bytes, b, sizeof(b));
    set_text(U.rx, s.on ? b : "\xE2\x80\x94");
    bytes_text(s.tx_bytes, b, sizeof(b));
    set_text(U.tx, s.on ? b : "\xE2\x80\x94");
    if (toggle_on(U.auto_toggle) != s.auto_connect) {
        toggle_set(U.auto_toggle, s.auto_connect, false);
    }
}

lv_obj_t *stat(lv_obj_t *card, const char *title, int x, int y)
{
    label(card, title, x, y, 170, TEXT3, 13);
    return label(card, "", x, y + 18, 170, TEXT, 19, true);
}

void build(lv_obj_t *s)
{
    U.screen = s;
    lv_obj_t *back = box(s, 56, 14, 44, 44, 22);
    set_bg_color(back, lv_color_hex(FIELD));
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    icon(back, "back", 10, 10, 24, TEXT, 2.0f);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    press_fx(back);
    lv_obj_add_event_cb(back, [](lv_event_t *) {
        if (U.hooks.exit) {
            U.hooks.exit();
        }
    }, LV_EVENT_CLICKED, nullptr);
    label(s, "VPN", 112, 20, 120, TEXT, 26, true);
    label(s, "WireGuard", 300, 27, 100, TEXT3, 15, false, LV_TEXT_ALIGN_RIGHT);
    U.dot = dot(s, 408, 32, 12, TEXT3);

    // The switch: the kit's toggle, large.
    constexpr float K = 3.2f;
    const int w = int(56 * K);
    vpn_summary_t now;
    vpn_app_summary(&now);
    U.shown_on = now.on;
    U.power = toggle(s, (PANEL - w) / 2, 80, now.on, INDIGO, K);
    lv_obj_add_event_cb(U.power, [](lv_event_t *) {
        const bool want = toggle_on(U.power);
        U.shown_on = want;
        if (U.hooks.haptic) {
            U.hooks.haptic();
        }
        vpn_app_connect(want);
        update();
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    U.state = label(s, "", 40, 200, 400, TEXT3, 30, true, LV_TEXT_ALIGN_CENTER);
    U.detail = label(s, "", 50, 242, 380, TEXT2, 16, false, LV_TEXT_ALIGN_CENTER);
    wrap_label(U.detail);

    U.card = glass(s, 40, 306, 400, 104, 22, INDIGO);
    lv_obj_remove_flag(U.card, LV_OBJ_FLAG_SCROLLABLE);
    U.address = stat(U.card, "Tunnel address", 20, 12);
    U.handshake = stat(U.card, "Last handshake", 20, 56);
    U.rx = stat(U.card, "Received", 214, 12);
    U.tx = stat(U.card, "Sent", 214, 56);

    label(s, "Connect at start-up", 76, 430, 260, TEXT2, 17);
    U.auto_toggle = toggle(s, 348, 424, now.auto_connect, INDIGO);
    lv_obj_add_event_cb(U.auto_toggle, [](lv_event_t *) {
        if (U.hooks.haptic) {
            U.hooks.haptic();
        }
        vpn_app_set_auto_connect(toggle_on(U.auto_toggle));
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    update();
    U.timer = lv_timer_create([](lv_timer_t *) {
        if (U.active) {
            update();
        }
    }, 500, nullptr);
    U.built = true;
}
}  // namespace

namespace vpn {
void ui_set_hooks(const vpn_hooks_t *hooks)
{
    if (hooks) {
        U.hooks = *hooks;
    }
}
}  // namespace vpn

extern "C" void vpn_app_open(lv_obj_t *screen)
{
    if (!U.built) {
        build(screen);
    }
    vpn_app_set_active(true);
}

extern "C" void vpn_app_set_active(bool active)
{
    U.active = active && U.built;
    if (U.active) {
        update();
    }
}
