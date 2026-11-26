// SPDX-License-Identifier: Apache-2.0
// The clocks' time zone: one of display_clock.h's zones (shared with Glimdock
// and Home as display/clockZone), chosen in Settings or found automatically
// from the network's location once Wi-Fi is up.
#include "time_zone.h"

#include <string.h>

#include <atomic>

#include "Preferences.h"
#include "WiFi.h"
#include "display_clock.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

namespace {
const char *TAG = "timezone";
constexpr const char *SHELL_PREFS = "mshell";

std::atomic<bool> automatic{true};
std::atomic<int> zone{0};
std::atomic<int> state{0};  // automatic: 0 not yet, 1 found, -1 lookup failed, -2 a zone the clock lacks
char found_name[48] = "";
TaskHandle_t task = nullptr;

// IANA names onto the clock's zones (the same rules: EU, US, none).
int zone_for(const char *name)
{
    static const char *const eet[] = {"Europe/Sofia", "Europe/Bucharest", "Europe/Athens", "Europe/Helsinki", "Europe/Kiev",
                                      "Europe/Kyiv", "Europe/Riga", "Europe/Tallinn", "Europe/Vilnius", "Asia/Nicosia",
                                      "Europe/Mariehamn", "Asia/Famagusta"
                                     };
    static const char *const gmt[] = {"Europe/London", "Europe/Dublin", "Europe/Lisbon", "Atlantic/Canary", "Atlantic/Madeira",
                                      "Atlantic/Faroe", "Europe/Guernsey", "Europe/Jersey", "Europe/Isle_of_Man"
                                     };
    static const char *const cet[] = {"Europe/Berlin", "Europe/Paris", "Europe/Madrid", "Europe/Rome", "Europe/Amsterdam",
                                      "Europe/Brussels", "Europe/Vienna", "Europe/Zurich", "Europe/Prague", "Europe/Warsaw",
                                      "Europe/Budapest", "Europe/Stockholm", "Europe/Oslo", "Europe/Copenhagen",
                                      "Europe/Belgrade", "Europe/Zagreb", "Europe/Ljubljana", "Europe/Bratislava",
                                      "Europe/Luxembourg", "Europe/Monaco", "Europe/Malta", "Europe/Andorra",
                                      "Europe/San_Marino", "Europe/Vatican", "Europe/Sarajevo", "Europe/Skopje",
                                      "Europe/Podgorica", "Europe/Tirane", "Europe/Vaduz", "Europe/Gibraltar",
                                      "Europe/Busingen", "Africa/Ceuta", "Arctic/Longyearbyen"
                                     };
    static const char *const est[] = {"America/New_York", "America/Toronto", "America/Detroit", "America/Montreal",
                                      "America/Nassau", "America/Iqaluit", "America/Nipigon", "America/Thunder_Bay",
                                      "America/Indiana/Indianapolis", "America/Kentucky/Louisville"
                                     };
    static const char *const pst[] = {"America/Los_Angeles", "America/Vancouver", "America/Tijuana"};
    auto in = [&](const char *const * list, size_t n) {
        for (size_t i = 0; i < n; ++i)
            if (!strcmp(name, list[i])) {
                return true;
            }
        return false;
    };
    if (!strcmp(name, "UTC") || !strcmp(name, "Etc/UTC") || !strcmp(name, "Etc/GMT") || !strcmp(name, "GMT")) {
        return 0;
    }
    if (in(eet, sizeof(eet) / sizeof(*eet))) {
        return 1;
    }
    if (in(gmt, sizeof(gmt) / sizeof(*gmt))) {
        return 2;
    }
    if (in(cet, sizeof(cet) / sizeof(*cet))) {
        return 3;
    }
    if (in(est, sizeof(est) / sizeof(*est))) {
        return 4;
    }
    if (in(pst, sizeof(pst) / sizeof(*pst))) {
        return 5;
    }
    if (!strcmp(name, "Asia/Tokyo")) {
        return 6;
    }
    return -1;
}

void save_zone(int z)
{
    Preferences p;
    if (p.begin("display", false)) {  // Glimdock's and Home's clocks read it too
        p.putUChar("clockZone", uint8_t(z));
        p.end();
    }
}

// The zone name for the public address this network uses (ipapi.co: plain
// text such as "Europe/Sofia").
bool look_up(char *out, size_t capacity)
{
    esp_http_client_config_t cfg = {};
    cfg.url = "https://ipapi.co/timezone/";
    cfg.timeout_ms = 8000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "mosaico-clock";
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        int n = esp_http_client_read(c, out, int(capacity) - 1);
        if (status == 200 && n > 0) {
            out[n] = 0;
            while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
                out[--n] = 0;
            }
            ok = n > 2 && strchr(out, '/') && !strchr(out, '<');
        } else {
            ESP_LOGW(TAG, "lookup: HTTP %d", status);
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

void worker(void *)
{
    TickType_t next = 0;
    for (;;) {
        // Woken by a connection or the switch to automatic; else twice a day.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(state == 1 ? 60000 : 10000));
        if (!automatic || WiFi.status() != WL_CONNECTED) {
            continue;
        }
        // Not before the next try is due, whatever the last one gave (a failed
        // lookup used to retry every 10 s, an HTTPS request each time).
        if (state != 0 && int32_t(xTaskGetTickCount() - next) < 0) {
            continue;
        }
        char name[48];
        if (!look_up(name, sizeof(name))) {
            state = -1;
            next = xTaskGetTickCount() + pdMS_TO_TICKS(5 * 60000);  // try again in a while
            ESP_LOGW(TAG, "time zone lookup failed");
            continue;
        }
        strlcpy(found_name, name, sizeof(found_name));
        const int z = zone_for(name);
        if (z < 0) {
            state = -2;
            ESP_LOGW(TAG, "%s is not one of the clock's zones; keeping %s", name, displayClockZoneName(uint8_t(zone.load())));
        } else {
            state = 1;
            if (z != zone) {
                zone = z;
                save_zone(z);
            }
            ESP_LOGI(TAG, "time zone %s: %s", name, displayClockZoneName(uint8_t(z)));
        }
        next = xTaskGetTickCount() + pdMS_TO_TICKS(12 * 3600 * 1000);
    }
}
}  // namespace

void time_zone_start()
{
    Preferences p;
    if (p.begin("display", true)) {
        zone = p.getUChar("clockZone", 0) % DISPLAY_CLOCK_ZONE_COUNT;
        p.end();
    }
    if (p.begin(SHELL_PREFS, true)) {
        automatic = p.getUChar("tzauto", 1) != 0;
        p.end();
    }
    // Network only: the stack can live in PSRAM.
    xTaskCreatePinnedToCoreWithCaps(worker, "timezone", 6144, nullptr, 2, &task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void time_zone_connected()
{
    if (task) {
        xTaskNotifyGive(task);
    }
}

void time_zone_step(int step)
{
    // Choices: automatic, then each zone.
    const int choices = DISPLAY_CLOCK_ZONE_COUNT + 1;
    int choice = automatic ? 0 : zone + 1;
    choice = ((choice + step) % choices + choices) % choices;
    automatic = choice == 0;
    if (!automatic) {
        zone = choice - 1;
        save_zone(zone);
    }
    Preferences p;
    if (p.begin(SHELL_PREFS, false)) {
        p.putUChar("tzauto", automatic ? 1 : 0);
        p.end();
    }
    if (automatic) {
        state = 0;
        time_zone_connected();
    }
}

void time_zone_set(int choice)
{
    const int current = automatic ? 0 : zone + 1, want = choice < 0 ? 0 : choice % DISPLAY_CLOCK_ZONE_COUNT + 1;
    if (want != current) {
        time_zone_step(want - current);
    }
}

const char *time_zone_name(int z)
{
    return displayClockZoneName(uint8_t(z));
}
int time_zone_count()
{
    return DISPLAY_CLOCK_ZONE_COUNT;
}

void time_zone_text(char *out, size_t capacity)
{
    const char *name = displayClockZoneName(uint8_t(zone.load()));
    if (!automatic) {
        strlcpy(out, name, capacity);
        return;
    }
    switch (state.load()) {
    case 1: snprintf(out, capacity, "Automatic \xC2\xB7 %s", name); break;
    case -1: snprintf(out, capacity, "Automatic \xC2\xB7 %s (offline)", name); break;
    case -2: snprintf(out, capacity, "Automatic \xC2\xB7 %s", name); break;
    default: snprintf(out, capacity, "Automatic \xC2\xB7 finding\xE2\x80\xA6"); break;
    }
}
