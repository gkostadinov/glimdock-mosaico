// SPDX-License-Identifier: Apache-2.0
// Settings > Wi-Fi in the desktop preview: made-up networks, a scan that takes
// a moment, and joins that work (or turn a password down) without a radio.
#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "board_wifi.h"
#include "lvgl.h"

namespace {
std::string current = "HomeWiFi";
uint32_t scan_until = 0;
BoardWifiJoin join = BoardWifiJoin::Idle;
std::string joining;
uint32_t join_until = 0;
std::vector<std::string> saved = {"Studio"};
}  // namespace

void board_wifi_start() {}

void board_wifi_poll()
{
    if (join == BoardWifiJoin::Joining && lv_tick_get() >= join_until) {
        join = BoardWifiJoin::Joined;
        current = joining;
    }
}

BoardWifiStatus board_wifi_status()
{
    board_wifi_poll();
    BoardWifiStatus s;
    s.ssid = current;
    s.connected = join != BoardWifiJoin::Joining;
    s.ip = "192.0.2.42";
    s.rssi = -48;
    s.vibe = current == "HomeWiFi";
    s.saved = !s.vibe;
    return s;
}

bool board_wifi_scan()
{
    scan_until = lv_tick_get() + 1200;
    return true;
}

bool board_wifi_scanning()
{
    return lv_tick_get() < scan_until;
}

std::vector<BoardWifiNetwork> board_wifi_results()
{
    if (board_wifi_scanning()) {
        return {};
    }
    auto known = [](const std::string & n) {
        for (const std::string &s : saved)
            if (s == n) {
                return true;
            }
        return n == "HomeWiFi";
    };
    std::vector<BoardWifiNetwork> out;
    for (const auto &[name, rssi, secure] : std::vector<std::tuple<const char *, int, bool>> {
    {"HomeWiFi", -48, true}, {"Studio", -61, true}, {"Cafe Guest", -69, false}, {"Neighbour 5G", -76, true},
    {"Workshop", -84, true}
}) {
        BoardWifiNetwork n;
        n.ssid = name;
        n.rssi = rssi;
        n.secure = secure;
        n.saved = known(name);
        n.vibe = n.ssid == "HomeWiFi";
        out.push_back(n);
    }
    return out;
}

void board_wifi_join(const char *ssid, const char *)
{
    joining = ssid;
    join = BoardWifiJoin::Joining;
    join_until = lv_tick_get() + 1500;
}

BoardWifiJoin board_wifi_join_state(std::string *ssid)
{
    board_wifi_poll();
    if (ssid) {
        *ssid = joining;
    }
    return join;
}

void board_wifi_join_seen()
{
    if (join != BoardWifiJoin::Joining) {
        join = BoardWifiJoin::Idle;
    }
}

bool board_wifi_forget(const char *ssid)
{
    saved.erase(std::remove(saved.begin(), saved.end(), std::string(ssid)), saved.end());
    return true;
}
