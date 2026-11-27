// SPDX-License-Identifier: Apache-2.0
// The board's Wi-Fi network, chosen in Settings > Wi-Fi. Vibe Mode's network
// (its shared information, which applications can only read) is the default;
// networks joined here are remembered (NVS "wifi", up to 8) and the board
// joins the one chosen last. When that one is out of range for a while it
// joins the strongest other network it knows, without changing the choice.
// Every application's WiFi.begin() joins this network (WiFi.setBoardNetwork).
// Calls run on the LVGL task, except board_wifi_start (start-up, before it).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

struct BoardWifiNetwork {
    std::string ssid;
    int rssi = 0;         // dBm; 0 when not seen in the last scan
    bool secure = false;  // needs a password
    bool saved = false;   // remembered here, or Vibe Mode's (no password needed)
    bool vibe = false;    // Vibe Mode's network
};

enum class BoardWifiJoin : uint8_t {
    Idle,
    Joining,
    Joined,
    WrongPassword,  // the network turned the password down
    NotFound,       // not in range (or the name is wrong)
    Failed,         // no address in time, or the network refused
};

struct BoardWifiStatus {
    std::string ssid;  // the network the board wants ("" none)
    bool connected = false;
    std::string ip;    // when connected
    int rssi = 0;      // when connected
    bool saved = false, vibe = false;  // the wanted network is remembered here / is Vibe Mode's
    bool fallback = false;  // on another known network while the chosen one is away
};

// Loads the networks and joins the chosen one (start-up, any task).
void board_wifi_start();
// Every 250 ms on the LVGL task: joins in progress, saving, the fallback.
void board_wifi_poll();
BoardWifiStatus board_wifi_status();

// A scan in the background; scanning() is true until results() has them
// (strongest first, one row per name; known networks marked). False: the
// radio is busy (try again shortly).
bool board_wifi_scan();
bool board_wifi_scanning();
std::vector<BoardWifiNetwork> board_wifi_results();

// Joins a network. password nullptr: the saved one (or Vibe Mode's, or none
// for an open network). On success it is remembered and chosen; on failure
// the board goes back to the network it was on.
void board_wifi_join(const char *ssid, const char *password);
BoardWifiJoin board_wifi_join_state(std::string *ssid = nullptr);
// After a result was shown: back to Idle.
void board_wifi_join_seen();
// Forgets a remembered network (Vibe Mode's cannot be: it is Vibe Mode's).
// If the board was on it, it moves to the next network it knows.
bool board_wifi_forget(const char *ssid);
