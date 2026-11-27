// SPDX-License-Identifier: Apache-2.0
// The board's Wi-Fi network (see board_wifi.h). Everything but the event
// handler runs on the LVGL task (or at start-up before it), so the state needs
// no lock; the handler only counts disconnect reasons and flags a finished
// scan. Saving is a flash write, so it happens here on the LVGL task and never
// on a task with its stack in PSRAM.
#include "board_wifi.h"

#include <string.h>

#include <algorithm>
#include <atomic>

#include "WiFi.h"
#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "glimdock_app.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

namespace {
const char *TAG = "board-wifi";
constexpr const char *NS = "wifi";
constexpr size_t MAX_SAVED = 8, MAX_RESULTS = 24;
constexpr int64_t JOIN_TIMEOUT_US = 25 * 1000000LL;
constexpr int64_t OFFLINE_BEFORE_FALLBACK_US = 30 * 1000000LL;
constexpr int64_t FALLBACK_RETRY_US = 90 * 1000000LL;
constexpr int64_t RETURN_CHECK_US = 3 * 60 * 1000000LL;
constexpr int64_t SCAN_START_TIMEOUT_US = 4 * 1000000LL, SCAN_TIMEOUT_US = 10 * 1000000LL;

struct Known {
    std::string ssid, password;
};

void wipe(std::string &s)
{
    if (!s.empty()) {
        mbedtls_platform_zeroize(&s[0], s.size());
    }
    s.clear();
}

std::vector<Known> saved;  // most recently joined first
std::string chosen;        // the network picked last (NVS "use")
Known vibe;                // Vibe Mode's network, if it has one
Known want;                // the network the board is on or joining
bool fallback = false;     // want is not the chosen one (it was away)

// A join from Settings.
BoardWifiJoin join_state = BoardWifiJoin::Idle;
Known joining, previous;
bool previous_fallback = false;
int64_t join_deadline = 0;
std::atomic<int> auth_failures{0}, not_found{0};

// Scans: for Settings, or to find another known network.
enum class Scan : uint8_t { None, Starting, Running };
Scan scan = Scan::None;
bool scan_for_fallback = false, scan_held = false;
int64_t scan_since = 0;
std::atomic<bool> scan_done{false};
std::vector<BoardWifiNetwork> results;

int64_t offline_since = 0, next_fallback = 0, next_return_check = 0;

int64_t now_us()
{
    return esp_timer_get_time();
}

bool linked()
{
    return WiFi.status() == WL_CONNECTED && uint32_t(WiFi.localIP()) != 0;
}

const Known *find_saved(const std::string &ssid)
{
    for (const Known &k : saved)
        if (k.ssid == ssid) {
            return &k;
        }
    return nullptr;
}

// The password to use for a known network: saved here first (it may be newer
// than Vibe Mode's), then Vibe Mode's.
const Known *known(const std::string &ssid)
{
    if (const Known *k = find_saved(ssid)) {
        return k;
    }
    return !vibe.ssid.empty() && vibe.ssid == ssid ? &vibe : nullptr;
}

void on_event(void *, esp_event_base_t, int32_t id, void *data)
{
    if (id == WIFI_EVENT_SCAN_DONE) {
        scan_done = true;
        return;
    }
    if (id != WIFI_EVENT_STA_DISCONNECTED || !data) {
        return;
    }
    switch (static_cast<wifi_event_sta_disconnected_t *>(data)->reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        auth_failures.fetch_add(1);
        break;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        not_found.fetch_add(1);
        break;
    default:
        break;
    }
}

// ---- storage ------------------------------------------------------------------------
// "nets": the saved networks as ssid NUL password NUL ..., most recent first;
// "use": the chosen network's name.
void load()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t size = 0;
    if (nvs_get_blob(h, "nets", nullptr, &size) == ESP_OK && size > 0 && size <= MAX_SAVED * 100) {
        std::string blob(size, '\0');
        if (nvs_get_blob(h, "nets", &blob[0], &size) == ESP_OK) {
            size_t at = 0;
            while (at < blob.size() && saved.size() < MAX_SAVED) {
                const size_t a = blob.find('\0', at);
                const size_t b = a == std::string::npos ? a : blob.find('\0', a + 1);
                if (b == std::string::npos) {
                    break;
                }
                Known k{blob.substr(at, a - at), blob.substr(a + 1, b - a - 1)};
                if (!k.ssid.empty() && k.ssid.size() <= 32 && k.password.size() <= 64) {
                    saved.push_back(std::move(k));
                }
                at = b + 1;
            }
        }
        wipe(blob);
    }
    char use[33] = "";
    size = sizeof(use);
    if (nvs_get_str(h, "use", use, &size) == ESP_OK) {
        chosen = use;
    }
    nvs_close(h);
}

void store()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no storage for the Wi-Fi networks");
        return;
    }
    std::string blob;
    for (const Known &k : saved) {
        blob += k.ssid;
        blob += '\0';
        blob += k.password;
        blob += '\0';
    }
    esp_err_t err = blob.empty() ? nvs_erase_key(h, "nets") : nvs_set_blob(h, "nets", blob.data(), blob.size());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    wipe(blob);
    if (err == ESP_OK) {
        err = chosen.empty() ? nvs_erase_key(h, "use") : nvs_set_str(h, "use", chosen.c_str());
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving the Wi-Fi networks: %s", esp_err_to_name(err));
    }
}

// ---- joining ------------------------------------------------------------------------
void end_scan();

// Moves the board (every app) to a network.
void apply(const Known &k)
{
    if (scan != Scan::None) {
        end_scan();
    }
    if (&k != &want) {
        wipe(want.ssid);
        wipe(want.password);
        want = k;
    }
    if (want.ssid.empty()) {
        WiFi.setBoardNetwork(nullptr, nullptr);
        glimdock_app_set_platform_wifi("", "");
        WiFi.disconnect();
        return;
    }
    WiFi.setBoardNetwork(want.ssid.c_str(), want.password.c_str());
    glimdock_app_set_platform_wifi(want.ssid.c_str(), want.password.c_str());
    WiFi.begin(want.ssid.c_str(), want.password.c_str());
    offline_since = now_us();
}

// The network to start on: the chosen one, else the last saved, else Vibe Mode's.
Known first_choice()
{
    if (!chosen.empty())
        if (const Known *k = known(chosen)) {
            return *k;
        }
    if (!saved.empty()) {
        return saved.front();
    }
    return vibe;
}

void remember(const Known &k)
{
    // Vibe Mode's own network with its own password needs no copy here.
    const bool is_vibe = !vibe.ssid.empty() && k.ssid == vibe.ssid && k.password == vibe.password;
    auto it = std::find_if(saved.begin(), saved.end(), [&](const Known & s) {
        return s.ssid == k.ssid;
    });
    if (it != saved.end()) {
        wipe(it->password);
        saved.erase(it);
    }
    if (!is_vibe) {
        saved.insert(saved.begin(), k);
        while (saved.size() > MAX_SAVED) {
            wipe(saved.back().password);
            saved.pop_back();
        }
    }
    chosen = k.ssid;
    store();
}

void finish_join(BoardWifiJoin result)
{
    join_state = result;
    if (result == BoardWifiJoin::Joined) {
        remember(joining);
        fallback = false;
        ESP_LOGI(TAG, "joined the network chosen in Settings");
    } else {
        ESP_LOGW(TAG, "joining failed (%d); back to the previous network", int(result));
        fallback = previous_fallback;
        apply(previous);
    }
    wipe(joining.password);
    wipe(previous.password);
}

// ---- scanning -----------------------------------------------------------------------
bool start_scan(bool for_fallback)
{
    if (join_state == BoardWifiJoin::Joining) {
        return false;
    }
    if (scan != Scan::None) {
        return true;
    }
    scan_for_fallback = for_fallback;
    scan_done = false;
    scan_since = now_us();
    scan = Scan::Starting;
    // A station busy connecting cannot scan: pause its retries meanwhile.
    if (!linked()) {
        scan_held = true;
        WiFi.holdReconnect(true);
        esp_wifi_disconnect();
    }
    return true;
}

// Lets the station retry again (at once, if it is not linked).
void end_scan()
{
    if (scan == Scan::Running && !scan_done) {
        esp_wifi_scan_stop();
    }
    scan = Scan::None;
    if (scan_held) {
        scan_held = false;
        WiFi.holdReconnect(false);
    }
}

void collect()
{
    uint16_t count = MAX_RESULTS;
    std::vector<wifi_ap_record_t> records(MAX_RESULTS);
    if (esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        count = 0;
    }
    results.clear();
    for (uint16_t i = 0; i < count; ++i) {
        const wifi_ap_record_t &r = records[i];
        const char *name = reinterpret_cast<const char *>(r.ssid);
        if (!name[0]) {
            continue;  // hidden
        }
        auto it = std::find_if(results.begin(), results.end(), [&](const BoardWifiNetwork & n) {
            return n.ssid == name;
        });
        if (it != results.end()) {
            it->rssi = std::max(it->rssi, int(r.rssi));
            continue;
        }
        BoardWifiNetwork n;
        n.ssid = name;
        n.rssi = r.rssi;
        n.secure = r.authmode != WIFI_AUTH_OPEN && r.authmode != WIFI_AUTH_OWE;
        n.saved = known(n.ssid) != nullptr;
        n.vibe = !vibe.ssid.empty() && n.ssid == vibe.ssid;
        results.push_back(std::move(n));
    }
    std::sort(results.begin(), results.end(), [](const BoardWifiNetwork & a, const BoardWifiNetwork & b) {
        return a.rssi > b.rssi;
    });
}

// After a scan for the fallback: the strongest known network in range.
void pick_fallback()
{
    const BoardWifiNetwork *best = nullptr;
    for (const BoardWifiNetwork &n : results) {
        if (n.saved && (!best || n.rssi > best->rssi)) {
            best = &n;
        }
    }
    if (!best) {
        return;
    }
    // The chosen network, when in range, wins over a stronger one.
    for (const BoardWifiNetwork &n : results)
        if (n.ssid == chosen && n.saved && n.rssi > -85) {
            best = &n;
        }
    if (best->ssid == want.ssid) {
        return;  // end_scan() let it retry
    }
    if (linked() && best->ssid != chosen) {
        return;  // a check for the chosen one coming back; stay
    }
    if (const Known *k = known(best->ssid)) {
        ESP_LOGI(TAG, "%s", best->ssid == chosen ? "back on the chosen network" : "on another known network");
        fallback = best->ssid != chosen;
        apply(Known(*k));
    }
}

void poll_scan()
{
    const int64_t now = now_us();
    if (scan == Scan::Starting) {
        wifi_scan_config_t config{};
        config.show_hidden = false;
        const esp_err_t err = esp_wifi_scan_start(&config, false);
        if (err == ESP_OK) {
            scan = Scan::Running;
            scan_since = now;
        } else if (now - scan_since > SCAN_START_TIMEOUT_US) {
            ESP_LOGW(TAG, "scan did not start: %s", esp_err_to_name(err));
            end_scan();
        }
        return;
    }
    if (scan != Scan::Running) {
        return;
    }
    if (scan_done) {
        collect();
        const bool for_fallback = scan_for_fallback;
        end_scan();
        if (for_fallback) {
            pick_fallback();
        }
    } else if (now - scan_since > SCAN_TIMEOUT_US) {
        end_scan();
    }
}
}  // namespace

// ---- the API --------------------------------------------------------------------------
void board_wifi_start()
{
    load();
    app_config_wifi_t shared;
    if (app_config_load_wifi(&shared) == ESP_OK) {
        vibe = {shared.ssid, shared.password};
    }
    mbedtls_platform_zeroize(&shared, sizeof(shared));
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, nullptr);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, on_event, nullptr);
    const Known first = first_choice();
    ESP_LOGI(TAG, "%zu saved networks, Vibe Mode's %s; joining %s", saved.size(), vibe.ssid.empty() ? "unset" : "set",
             first.ssid.empty() ? "none" : first.ssid == vibe.ssid && !find_saved(first.ssid) ? "Vibe Mode's" : "a saved one");
    if (!first.ssid.empty()) {
        apply(first);
    }
}

void board_wifi_poll()
{
    const int64_t now = now_us();
    poll_scan();
    if (join_state == BoardWifiJoin::Joining) {
        if (linked() && !strcmp(WiFi.SSID().c_str(), joining.ssid.c_str())) {
            finish_join(BoardWifiJoin::Joined);
        } else if (auth_failures >= 2) {
            finish_join(BoardWifiJoin::WrongPassword);
        } else if (not_found >= 3) {
            finish_join(BoardWifiJoin::NotFound);
        } else if (now > join_deadline) {
            finish_join(BoardWifiJoin::Failed);
        }
        return;
    }
    if (want.ssid.empty()) {
        return;
    }
    if (linked()) {
        offline_since = 0;
        next_fallback = 0;
        // On another network while the chosen one was away: look for it now and then.
        if (fallback && scan == Scan::None && now > next_return_check) {
            next_return_check = now + RETURN_CHECK_US;
            start_scan(true);
        }
        return;
    }
    if (!offline_since) {
        offline_since = now;
    }
    // Away from the network for a while: another one it knows may be in range.
    const size_t candidates = saved.size() + (!vibe.ssid.empty() && !find_saved(vibe.ssid) ? 1 : 0);
    if (candidates > 1 && scan == Scan::None && now - offline_since > OFFLINE_BEFORE_FALLBACK_US && now > next_fallback) {
        next_fallback = now + FALLBACK_RETRY_US;
        start_scan(true);
    }
}

BoardWifiStatus board_wifi_status()
{
    BoardWifiStatus s;
    s.ssid = want.ssid;
    s.connected = !want.ssid.empty() && linked();
    if (s.connected) {
        s.ip = WiFi.localIP().toString().c_str();
        s.rssi = WiFi.RSSI();
    }
    s.saved = find_saved(want.ssid) != nullptr;
    s.vibe = !s.saved && !vibe.ssid.empty() && want.ssid == vibe.ssid;
    s.fallback = fallback;
    return s;
}

bool board_wifi_scan()
{
    return start_scan(false);
}

bool board_wifi_scanning()
{
    return scan != Scan::None;
}

std::vector<BoardWifiNetwork> board_wifi_results()
{
    for (BoardWifiNetwork &n : results) {  // a join or forget since may have changed these
        n.saved = known(n.ssid) != nullptr;
    }
    return results;
}

void board_wifi_join(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || join_state == BoardWifiJoin::Joining) {
        return;
    }
    const Known *k = known(ssid);
    previous = want;
    previous_fallback = fallback;
    joining = {ssid, password ? password : k ? k->password : ""};
    auth_failures = 0;
    not_found = 0;
    join_deadline = now_us() + JOIN_TIMEOUT_US;
    join_state = BoardWifiJoin::Joining;
    apply(Known(joining));
}

BoardWifiJoin board_wifi_join_state(std::string *ssid)
{
    if (ssid) {
        *ssid = joining.ssid;
    }
    return join_state;
}

void board_wifi_join_seen()
{
    if (join_state != BoardWifiJoin::Joining) {
        join_state = BoardWifiJoin::Idle;
    }
}

bool board_wifi_forget(const char *ssid)
{
    auto it = std::find_if(saved.begin(), saved.end(), [&](const Known & k) {
        return k.ssid == ssid;
    });
    if (!ssid || it == saved.end() || join_state == BoardWifiJoin::Joining) {
        return false;
    }
    const std::string name = ssid;  // ssid may point into the entry
    wipe(it->password);
    saved.erase(it);
    if (chosen == name) {
        chosen = !saved.empty() ? saved.front().ssid : vibe.ssid;
    }
    store();
    if (want.ssid == name) {
        fallback = false;
        apply(first_choice());
    }
    return true;
}
