#pragma once
// Optional private build defaults. Saved setup settings take precedence.
#if !defined(GLIMDOCK_PUBLIC_FIRMWARE) && __has_include("local_credentials.h")
#include "local_credentials.h"
#endif
#ifdef GLIMDOCK_PUBLIC_FIRMWARE
// Redistributable builds always begin unpaired; NVS settings survive updates.
#undef HOMELAB_DEFAULT_SSID
#undef HOMELAB_DEFAULT_PASSWORD
#undef HOMELAB_DEFAULT_TOKEN
#undef HOMELAB_DEFAULT_SETUP_TOKEN
#undef HOMELAB_DEFAULT_ENDPOINT
#ifdef HOMELAB_PLATFORM_WIFI
// The board platform provisions Wi-Fi outside Glimdock (ESP-Mosaico Vibe Mode
// shared information). Glimdock settings saved later still take precedence.
const char* homelabPlatformSsid();
const char* homelabPlatformPassword();
#define HOMELAB_DEFAULT_SSID homelabPlatformSsid()
#define HOMELAB_DEFAULT_PASSWORD homelabPlatformPassword()
#else
#define HOMELAB_DEFAULT_SSID ""
#define HOMELAB_DEFAULT_PASSWORD ""
#endif
#define HOMELAB_DEFAULT_TOKEN ""
#define HOMELAB_DEFAULT_SETUP_TOKEN ""
#define HOMELAB_DEFAULT_ENDPOINT ""
#endif
#ifndef HOMELAB_DEFAULT_SSID
#define HOMELAB_DEFAULT_SSID ""
#endif
#ifndef HOMELAB_DEFAULT_PASSWORD
#define HOMELAB_DEFAULT_PASSWORD ""
#endif
#ifndef HOMELAB_DEFAULT_TOKEN
#define HOMELAB_DEFAULT_TOKEN ""
#endif
#ifndef HOMELAB_DEFAULT_SETUP_TOKEN
#define HOMELAB_DEFAULT_SETUP_TOKEN ""
#endif
#ifndef HOMELAB_DEFAULT_ENDPOINT
#define HOMELAB_DEFAULT_ENDPOINT ""
#endif
#define HOMELAB_POLL_MS 3000
#define HOMELAB_STALE_MS 15000
#define HOMELAB_IDLE_DIM_MS 120000
#define HOMELAB_AUTO_PAGE_MS 15000
// 0/2: portrait 240x320; 1/3: landscape 320x240. Display and touch rotate together.
// Pebble Landscape uses USB-right rotation 3. Explicit build environments
// preserve portrait and alternate landscape orientations.
#ifndef HOMELAB_ROTATION
#define HOMELAB_ROTATION 3
#endif
// Physical RGB bars test at boot; no networking is required. Default is off.
#ifndef HOMELAB_COLOR_TEST
#define HOMELAB_COLOR_TEST 0
#endif
// Board features. Defaults describe the Waveshare ESP32-S3 Touch LCD 2.8 V1;
// other boards (ESP-Mosaico) override them at build time.
// HOMELAB_NO_FLIPPER also selects the inert Flipper stubs in flipper_*.h.
#ifndef HOMELAB_FLIPPER
#ifdef HOMELAB_NO_FLIPPER
#define HOMELAB_FLIPPER 0
#else
#define HOMELAB_FLIPPER 1
#endif
#endif
// Phone setup portal (Wi-Fi hotspot + captive page) when Glimdock is unconfigured.
#ifndef HOMELAB_SETUP_PORTAL
#define HOMELAB_SETUP_PORTAL 1
#endif
// Network worker stack (manageNodes() keeps a ~21 KB settings frame).
#ifndef HOMELAB_NETWORK_STACK
#define HOMELAB_NETWORK_STACK 24576
#endif
#ifndef HOMELAB_POWER_BUTTON
#define HOMELAB_POWER_BUTTON 1
#endif
#ifndef HOMELAB_BOARD_NAME
#define HOMELAB_BOARD_NAME "Waveshare ESP32-S3 Touch LCD 2.8 V1"
#endif
#ifndef HOMELAB_BATTERY_NOTE
#define HOMELAB_BATTERY_NOTE "3.7 V LiPo \xE2\x80\xA2 approximate voltage"
#endif
