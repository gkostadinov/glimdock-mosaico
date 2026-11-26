// SPDX-License-Identifier: Apache-2.0
// Glimdock (the homelab monitor) as one application of the ESP-Mosaico shell. All calls run on the LVGL task (or with its lock held).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wi-Fi provisioned by the platform (Vibe Mode). Glimdock uses it until its
// own settings are saved. Copied; the caller clears its buffers.
void glimdock_app_set_platform_wifi(const char *ssid, const char *password);

// First call builds the UI on screen (a full-panel LVGL screen) and starts
// the network. Nothing is shown until the shell loads that screen.
void glimdock_app_start(lv_obj_t *screen);
bool glimdock_app_started(void);
// Starts if needed and makes Glimdock the active application.
void glimdock_app_open(lv_obj_t *screen);
// While inactive the UI is not rebuilt; the network keeps polling.
void glimdock_app_set_active(bool active);
// Opens a Glimdock page (0 Systems, 1 Clock, 2 Settings, 3 Overview, ... 10 Agents).
void glimdock_app_select(int page);
// Touch transitions (kept for the shell; unused by the Mosaico UI).
void glimdock_app_note_touch(bool pressed);

// What the launcher shows on the Glimdock page. Values are NAN when unknown.
typedef struct {
    bool started, ready, fresh;
    int nodes, healthy, alerts;  // nodes = 0: a single system
    float cpu, memory;
    float history[32];           // fleet CPU, oldest first
    int history_count;
    struct {
        char name[28], note[20];
        uint8_t state;  // 0 healthy, 1 degraded or stale, 2 offline
    } items[3];
    int item_count;
    char status[72];
} glimdock_summary_t;
void glimdock_app_summary(glimdock_summary_t *out);

// For Swifty: the homelab as text. topic: "fleet" (every system), "system"
// (the one shown: load, memory, guests, storage, disks, alerts), "agents"
// (Kordlet cards and sessions), or empty for all of it, briefly. Any task.
void glimdock_app_report(const char *topic, char *out, size_t capacity);

// Connection settings, stored as one set the way the setup portal stores them
// (NVS "homelab"). A null field keeps the saved value; with no Wi-Fi saved or
// given, Vibe Mode's network is used. Validated as the portal validates
// (an https:// endpoint needs its CA). Applies after a restart. Returns null,
// or why the settings were refused.
typedef struct {
    const char *ssid, *password, *endpoint, *token, *setup_token, *ca;
} glimdock_connection_t;
const char *glimdock_app_store_connection(const glimdock_connection_t *settings);
typedef struct {
    char ssid[33], endpoint[241];
    bool password, token, setup_token, ca;
} glimdock_connection_info_t;
void glimdock_app_connection_info(glimdock_connection_info_t *out);

#ifdef __cplusplus
}
#endif
