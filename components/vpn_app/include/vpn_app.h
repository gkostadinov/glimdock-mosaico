// SPDX-License-Identifier: Apache-2.0
// VPN: a WireGuard tunnel that carries the board's internet traffic while it
// is on. The tunnel becomes the default route and DNS goes through it; the
// local network stays direct, and the tunnel's own packets go out over Wi-Fi.
// Settings come from tools/mosaico-config.py or Web settings (the "vpn"
// section: fields or a whole WireGuard .conf) and are kept in NVS.
//
// UI calls run on the LVGL task (or with its lock held); the others are safe
// from any task.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*exit)(void);
    void (*haptic)(void);
} vpn_hooks_t;

typedef enum {
    VPN_OFF = 0,
    VPN_NOT_SET_UP,  // switched on without a usable configuration (see error)
    VPN_WAITING,     // for Wi-Fi
    VPN_CONNECTING,  // the first handshake, or again after one was lost
    VPN_CONNECTED,
    VPN_ERROR,  // still trying; error says what is wrong
} vpn_state_t;

typedef struct {
    bool configured;  // a complete, valid configuration is saved
    bool on;          // switched on (the person's wish; see state)
    bool auto_connect;
    bool full_tunnel;  // all internet traffic (AllowedIPs 0.0.0.0/0)
    bool tunnel_dns;   // DNS goes through the tunnel
    vpn_state_t state;
    char address[20];   // the tunnel address, "10.8.0.2"
    char endpoint[72];  // as configured, "vpn.example.com:51820"
    char error[120];    // in plain words; empty when there is nothing wrong
    int32_t handshake_age_s;  // since the last handshake; -1 none yet
    uint16_t mtu;             // the tunnel's (configured, or 1280)
    uint64_t rx_bytes, tx_bytes;  // this connection's, as WireGuard sends them
} vpn_summary_t;

// Loads the settings and connects when auto-connect is on. Call once, after
// Wi-Fi has been started.
void vpn_app_init(const vpn_hooks_t *hooks);
void vpn_app_open(lv_obj_t *screen);
void vpn_app_set_active(bool active);

void vpn_app_summary(vpn_summary_t *out);
// Switch the tunnel; returns at once (the summary follows the connection).
void vpn_app_connect(bool on);
void vpn_app_set_auto_connect(bool on);  // saves it (LVGL task)
// The state in a sentence, for Swifty and the logs.
void vpn_app_report(char *out, size_t capacity);

// The "vpn" section of the board configuration (swifty_app_config_section):
// apply returns NULL or an error; status never includes a key.
const char *vpn_app_config_apply(const char *json, size_t length);
void vpn_app_config_status(char *json, size_t capacity);

#ifdef __cplusplus
}

namespace mui {
struct Accent;
}
// The VPN's colour (indigo), for its home screen icon.
extern const mui::Accent vpn_accent;
#endif
