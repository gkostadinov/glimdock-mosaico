// SPDX-License-Identifier: Apache-2.0
// The clocks' time zone: one of display_clock.h's zones (shared with Glimdock
// and Home as display/clockZone), chosen in Settings or found automatically
// from the network's location once Wi-Fi is up. The lookup's coordinates and
// city are kept for the weather (time_zone_location).
#include "time_zone.h"

#include <string.h>

#include <math.h>

#include <atomic>
#include <mutex>
#include <string>

#include "ArduinoJson.h"
#include "Preferences.h"
#include "WiFi.h"
#include "bsp/esp_mosaico.h"
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
std::atomic<bool> want_location{false};  // the weather asked for the location
std::atomic<bool> due_now{true};          // look up at the next chance, whenever the last one was
std::mutex where_lock;                     // guards where
struct {
    bool known = false;
    float lat = NAN, lon = NAN;
    char city[40] = "";
} where;

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

// The C library's local time (localtime_r, mktime) follows the clock's zone,
// with the same rules: Swifty's sense of the time, reminders and file times.
void apply_zone(int z)
{
    static const char *const POSIX[] = {"UTC0", "EET-2EEST,M3.5.0/3,M10.5.0/4", "GMT0BST,M3.5.0/1,M10.5.0",
                                        "CET-1CEST,M3.5.0,M10.5.0/3", "EST5EDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0", "JST-9"
                                       };
    static_assert(sizeof(POSIX) / sizeof(POSIX[0]) == DISPLAY_CLOCK_ZONE_COUNT, "a POSIX rule for each clock zone");
    setenv("TZ", POSIX[z >= 0 && z < DISPLAY_CLOCK_ZONE_COUNT ? z : 0], 1);
    tzset();
}

void save_zone(int z)
{
    apply_zone(z);
    Preferences p;
    if (p.begin("display", false)) {  // Glimdock's and Home's clocks read it too
        p.putUChar("clockZone", uint8_t(z));
        p.end();
    }
}

// The zone name for the public address this network uses ("Europe/Sofia"),
// and its city and coordinates for the weather: ipapi.co's JSON, one request.
bool look_up(char *out, size_t capacity)
{
    esp_http_client_config_t cfg = {};
    cfg.url = "https://ipapi.co/json/";
    cfg.timeout_ms = 8000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "mosaico-clock";
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    bool ok = false;
    std::string body;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        char buf[256];
        for (int n; body.size() < 4096 && (n = esp_http_client_read(c, buf, sizeof(buf))) > 0;) {
            body.append(buf, size_t(n));
        }
        if (status != 200) {
            ESP_LOGW(TAG, "lookup: HTTP %d", status);
        }
        JsonDocument doc;
        if (status == 200 && !deserializeJson(doc, body) && !(doc["error"] | false)) {
            const char *zone_name = doc["timezone"] | "";
            strlcpy(out, zone_name, capacity);
            ok = strlen(out) > 2 && strchr(out, '/');
            const float lat = doc["latitude"] | NAN, lon = doc["longitude"] | NAN;
            if (isfinite(lat) && isfinite(lon) && fabsf(lat) <= 90 && fabsf(lon) <= 180) {
                std::lock_guard<std::mutex> g(where_lock);
                where.known = true;
                where.lat = lat;
                where.lon = lon;
                strlcpy(where.city, doc["city"] | "", sizeof(where.city));
                ok = true;  // a place, if not a zone the clock has
            }
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
        if (!(automatic || want_location) || WiFi.status() != WL_CONNECTED) {
            continue;
        }
        // Not before the next try is due, whatever the last one gave (a failed
        // lookup used to retry every 10 s, an HTTPS request each time).
        if (!due_now && int32_t(xTaskGetTickCount() - next) < 0) {
            continue;
        }
        due_now = false;
        char name[48];
        if (!look_up(name, sizeof(name))) {
            state = -1;
            next = xTaskGetTickCount() + pdMS_TO_TICKS(5 * 60000);  // try again in a while
            ESP_LOGW(TAG, "time zone lookup failed");
            continue;
        }
        next = xTaskGetTickCount() + pdMS_TO_TICKS(12 * 3600 * 1000);
        if (!automatic) {  // only the weather wanted it: the zone stays as set
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
                apply_zone(z);
                // The NVS write goes to the LVGL task: this task's stack is in
                // PSRAM, which the cache can't reach while flash is written.
                if (bsp_display_lock(1000)) {
                    lv_async_call([](void *) {
                        save_zone(zone);
                    }, nullptr);
                    bsp_display_unlock();
                }
            }
            ESP_LOGI(TAG, "time zone %s: %s", name, displayClockZoneName(uint8_t(z)));
        }
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
    apply_zone(zone);
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
        due_now = true;
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

bool time_zone_location(float *latitude, float *longitude, char *city, size_t capacity)
{
    std::lock_guard<std::mutex> g(where_lock);
    if (!where.known) {
        return false;
    }
    *latitude = where.lat;
    *longitude = where.lon;
    strlcpy(city, where.city, capacity);
    return true;
}

void time_zone_want_location()
{
    if (!want_location.exchange(true)) {
        std::lock_guard<std::mutex> g(where_lock);
        if (!where.known) {  // a first lookup now, not at the next due time
            due_now = true;
            time_zone_connected();
        }
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
