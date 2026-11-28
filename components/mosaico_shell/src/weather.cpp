// SPDX-License-Identifier: Apache-2.0
// Weather (weather.h). Open-Meteo (api.open-meteo.com, free, no key) gives
// the forecast; its geocoding API finds a city typed in Settings once (the
// coordinates are saved with the name). The automatic place is the time
// zone's IP lookup (time_zone_location), so there is no second lookup.
//
// The fetching runs on its own task with its stack in PSRAM, at priority 2,
// below LVGL's drawing (network only: no flash writes there). The report lives
// behind a mutex held only to copy it; the LVGL task saves it and the settings
// to NVS from a timer, as reminders and shortcuts do.
#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

#include "ArduinoJson.h"
#include "WiFi.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "iris_ext.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "time_zone.h"

namespace {
const char *TAG = "weather";
const char *NS = "weather";
const TickType_t REFRESH = pdMS_TO_TICKS(30 * 60 * 1000), RETRY = pdMS_TO_TICKS(5 * 60 * 1000),
                 LOCATING = pdMS_TO_TICKS(15 * 1000);
constexpr time_t STALE_S = 2 * 3600;
constexpr size_t MAX_BODY = 12 * 1024;
constexpr uint8_t CACHE_VERSION = 1;

// The last forecast as NVS keeps it ("cache"), so it shows at boot.
struct Cache {
    uint8_t version;
    uint8_t code, day, hour_count;
    float temp, high, low;
    WeatherHour hours[WEATHER_HOURS];
    int64_t fetched;
    uint8_t automatic;  // for the network's location (then lat, lon stand in for it until the next lookup)
    float lat, lon;
    char place[40];
};

// A city's coordinates, found once for the name typed ("geo").
struct Geo {
    float lat, lon;
    char name[40];
    char typed[64];
};

struct Place {
    float lat = NAN, lon = NAN;
    char name[40] = "";
    bool automatic = true;
};

std::mutex lock;  // guards S, held only to copy in or out
struct {
    WeatherReport report;  // the forecast, its state; units and place mode from the settings
    char setting[64] = "";  // the place as set ("" automatic)
    Geo geo{NAN, NAN, "", ""};
    Place last_auto;  // the automatic place of the last forecast
    char error[72] = "";
    bool save_cache = false, save_settings = false;
} S;
std::atomic<bool> force{false};
TaskHandle_t task = nullptr;
bool started = false;

void set_state(WeatherState state, const char *error = nullptr)
{
    std::lock_guard<std::mutex> g(lock);
    S.report.state = state;
    if (error) {
        strlcpy(S.error, error, sizeof(S.error));
    }
}

// "lat,lon" with both in range.
bool parse_coords(const char *s, float *lat, float *lon)
{
    char *end = nullptr;
    const double a = strtod(s, &end);
    if (end == s) {
        return false;
    }
    while (*end == ' ') {
        ++end;
    }
    if (*end != ',') {
        return false;
    }
    const char *second = end + 1;
    const double b = strtod(second, &end);
    if (end == second) {
        return false;
    }
    while (*end == ' ') {
        ++end;
    }
    if (*end || !(a >= -90 && a <= 90) || !(b >= -180 && b <= 180)) {
        return false;
    }
    *lat = float(a);
    *lon = float(b);
    return true;
}

std::string url_encode(const char *s)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string out;
    for (; *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out += char(c);
        } else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 15];
        }
    }
    return out;
}

// GET url into body. The HTTP status, or -1 when there was no answer.
int get(const char *url, std::string &body)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "mosaico-weather";
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    int status = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        char buf[512];
        for (int n; (n = esp_http_client_read(c, buf, sizeof(buf))) > 0;) {
            if (body.size() + size_t(n) > MAX_BODY) {
                status = -1;
                break;
            }
            body.append(buf, size_t(n));
        }
    }
    esp_http_client_cleanup(c);
    return status;
}

// A city typed in Settings: "Lisbon", or "Springfield, US" (a country code,
// or the start of a country or region, after the comma).
enum class Found { Yes, No, Failed };
Found geocode(const char *typed, Place &p)
{
    std::string name = typed, within;
    const size_t comma = name.find(',');
    if (comma != std::string::npos) {
        within = name.substr(comma + 1);
        name.resize(comma);
        within.erase(0, within.find_first_not_of(' '));
    }
    const std::string url = "https://geocoding-api.open-meteo.com/v1/search?count=10&language=en&format=json&name=" +
                            url_encode(name.c_str());
    std::string body;
    const int status = get(url.c_str(), body);
    if (status != 200) {
        char e[48];
        snprintf(e, sizeof(e), "geocoding: HTTP %d", status);
        set_state(WeatherState::Failed, e);
        return Found::Failed;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        set_state(WeatherState::Failed, "geocoding: not JSON");
        return Found::Failed;
    }
    const auto starts = [](const char *text, const std::string & with) {
        return !with.empty() && text && strncasecmp(text, with.c_str(), with.size()) == 0;
    };
    for (JsonObject r : doc["results"].as<JsonArray>()) {
        if (!within.empty() && !(strcasecmp(r["country_code"] | "", within.c_str()) == 0 || starts(r["country"] | "", within) ||
                                 starts(r["admin1"] | "", within))) {
            continue;
        }
        p.lat = r["latitude"] | NAN;
        p.lon = r["longitude"] | NAN;
        strlcpy(p.name, r["name"] | typed, sizeof(p.name));
        p.automatic = false;
        return isfinite(p.lat) && isfinite(p.lon) ? Found::Yes : Found::No;
    }
    return Found::No;
}

// Where to fetch the forecast for. False with the reason in *why.
bool resolve(Place &p, WeatherState *why)
{
    char setting[64];
    Geo geo;
    Place last;
    {
        std::lock_guard<std::mutex> g(lock);
        strlcpy(setting, S.setting, sizeof(setting));
        geo = S.geo;
        last = S.last_auto;
    }
    if (!setting[0]) {
        if (time_zone_location(&p.lat, &p.lon, p.name, sizeof(p.name))) {
            p.automatic = true;
            return true;
        }
        time_zone_want_location();
        if (isfinite(last.lat)) {  // the last forecast's place, until the network's is known
            p = last;
            return true;
        }
        *why = WeatherState::Locating;
        return false;
    }
    if (parse_coords(setting, &p.lat, &p.lon)) {
        snprintf(p.name, sizeof(p.name), "%.2f\xC2\xB0, %.2f\xC2\xB0", double(p.lat), double(p.lon));
        p.automatic = false;
        return true;
    }
    if (!strcmp(geo.typed, setting) && isfinite(geo.lat)) {
        p.lat = geo.lat;
        p.lon = geo.lon;
        strlcpy(p.name, geo.name, sizeof(p.name));
        p.automatic = false;
        return true;
    }
    switch (geocode(setting, p)) {
    case Found::Yes: {
        std::lock_guard<std::mutex> g(lock);
        if (!strcmp(S.setting, setting)) {  // not changed meanwhile
            S.geo = {p.lat, p.lon, "", ""};
            strlcpy(S.geo.name, p.name, sizeof(S.geo.name));
            strlcpy(S.geo.typed, setting, sizeof(S.geo.typed));
            S.save_settings = true;
        }
        return true;
    }
    case Found::No:
        *why = WeatherState::NoPlace;
        ESP_LOGW(TAG, "the place set was not found");
        return false;
    default:
        *why = WeatherState::Failed;
        return false;
    }
}

// The forecast for p into r: now, today's high and low, the next hours.
bool forecast(const Place &p, WeatherReport &r)
{
    char url[360];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,is_day&hourly=temperature_2m,weather_code,is_day"
             "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=2&forecast_hours=%d",
             double(p.lat), double(p.lon), WEATHER_HOURS + 1);
    std::string body;
    const int status = get(url, body);
    if (status != 200) {
        char e[48];
        snprintf(e, sizeof(e), "forecast: HTTP %d", status);
        set_state(WeatherState::Failed, e);
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        set_state(WeatherState::Failed, "forecast: not JSON");
        return false;
    }
    JsonObject now = doc["current"];
    if (!now["temperature_2m"].is<float>()) {
        set_state(WeatherState::Failed, "forecast: no current weather");
        return false;
    }
    r.temp = now["temperature_2m"];
    r.code = uint8_t(now["weather_code"] | 0);
    r.day = (now["is_day"] | 1) != 0;
    r.high = doc["daily"]["temperature_2m_max"][0] | r.temp;
    r.low = doc["daily"]["temperature_2m_min"][0] | r.temp;
    // The hours after this one ("2026-11-28T21:00"; the current hour comes first).
    const char *current = now["time"] | "";
    JsonArray times = doc["hourly"]["time"], temps = doc["hourly"]["temperature_2m"],
              codes = doc["hourly"]["weather_code"], days = doc["hourly"]["is_day"];
    r.hour_count = 0;
    for (size_t i = 0; i < times.size() && r.hour_count < WEATHER_HOURS; ++i) {
        const char *t = times[i] | "";
        if (strlen(t) < 13 || !strncmp(t, current, 13)) {
            continue;
        }
        WeatherHour &h = r.hours[r.hour_count++];
        h.hour = int8_t(atoi(t + 11));
        h.temp10 = int16_t(lroundf((temps[i] | 0.0f) * 10));
        h.code = uint8_t(codes[i] | 0);
        h.day = (days[i] | 1) != 0;
    }
    return true;
}

void worker(void *)
{
    TickType_t next = 0;  // the next fetch; 0: as soon as online
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000));
        const bool forced = force.exchange(false);
        if (WiFi.status() != WL_CONNECTED) {
            set_state(WeatherState::Offline);
            continue;
        }
        if (!forced && next && int32_t(xTaskGetTickCount() - next) < 0) {
            continue;
        }
        Place place;
        WeatherState why = WeatherState::Failed;
        if (!resolve(place, &why)) {
            set_state(why);
            next = xTaskGetTickCount() + (why == WeatherState::Locating ? LOCATING : RETRY);
            continue;
        }
        set_state(WeatherState::Fetching);
        WeatherReport fresh;
        if (!forecast(place, fresh)) {
            char error[72];
            {
                std::lock_guard<std::mutex> g(lock);
                strlcpy(error, S.error, sizeof(error));
            }
            ESP_LOGW(TAG, "%s", error);
            next = xTaskGetTickCount() + RETRY;
            continue;
        }
        next = xTaskGetTickCount() + REFRESH;
        std::lock_guard<std::mutex> g(lock);
        WeatherReport &r = S.report;
        r.valid = true;
        r.state = WeatherState::Idle;
        r.temp = fresh.temp;
        r.high = fresh.high;
        r.low = fresh.low;
        r.code = fresh.code;
        r.day = fresh.day;
        memcpy(r.hours, fresh.hours, sizeof(r.hours));
        r.hour_count = fresh.hour_count;
        strlcpy(r.place, place.name, sizeof(r.place));
        r.fetched = time(nullptr);
        S.error[0] = 0;
        if (place.automatic) {
            S.last_auto = place;
        }
        S.save_cache = true;
        ESP_LOGI(TAG, "updated (%s place): code %d", place.automatic ? "automatic" : "set", r.code);
    }
}

// ---- NVS, from the LVGL task ------------------------------------------------------------
void load()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t units = 0;
    nvs_get_u8(h, "units", &units);
    S.report.fahrenheit = units != 0;
    size_t n = sizeof(S.setting);
    if (nvs_get_str(h, "place", S.setting, &n) != ESP_OK) {
        S.setting[0] = 0;
    }
    S.report.automatic = !S.setting[0];
    n = sizeof(S.geo);
    Geo geo;
    if (nvs_get_blob(h, "geo", &geo, &n) == ESP_OK && n == sizeof(geo)) {
        geo.name[sizeof(geo.name) - 1] = geo.typed[sizeof(geo.typed) - 1] = 0;
        S.geo = geo;
    }
    Cache c;
    n = sizeof(c);
    if (nvs_get_blob(h, "cache", &c, &n) == ESP_OK && n == sizeof(c) && c.version == CACHE_VERSION &&
            bool(c.automatic) == S.report.automatic) {
        WeatherReport &r = S.report;
        c.place[sizeof(c.place) - 1] = 0;
        r.valid = true;
        r.code = c.code;
        r.day = c.day != 0;
        r.temp = c.temp;
        r.high = c.high;
        r.low = c.low;
        r.hour_count = std::min<int>(c.hour_count, WEATHER_HOURS);
        memcpy(r.hours, c.hours, sizeof(r.hours));
        r.fetched = time_t(c.fetched);
        strlcpy(r.place, c.place, sizeof(r.place));
        if (c.automatic) {
            S.last_auto.lat = c.lat;
            S.last_auto.lon = c.lon;
            strlcpy(S.last_auto.name, c.place, sizeof(S.last_auto.name));
        }
    }
    nvs_close(h);
}

void save_cb(lv_timer_t *)
{
    Cache c{};
    char setting[64];
    Geo geo;
    bool fahrenheit, cache, drop, settings;
    {
        std::lock_guard<std::mutex> g(lock);
        cache = S.save_cache && S.report.valid;
        drop = S.save_cache && !S.report.valid;  // the place changed: the old forecast is no use at boot
        settings = S.save_settings;
        S.save_cache = S.save_settings = false;
        const WeatherReport &r = S.report;
        c.version = CACHE_VERSION;
        c.code = r.code;
        c.day = r.day;
        c.hour_count = uint8_t(r.hour_count);
        c.temp = r.temp;
        c.high = r.high;
        c.low = r.low;
        memcpy(c.hours, r.hours, sizeof(c.hours));
        c.fetched = int64_t(r.fetched);
        c.automatic = r.automatic;
        c.lat = S.last_auto.lat;
        c.lon = S.last_auto.lon;
        strlcpy(c.place, r.place, sizeof(c.place));
        strlcpy(setting, S.setting, sizeof(setting));
        geo = S.geo;
        fahrenheit = r.fahrenheit;
    }
    if (!cache && !drop && !settings) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (cache) {
        nvs_set_blob(h, "cache", &c, sizeof(c));
    } else if (drop) {
        nvs_erase_key(h, "cache");
    }
    if (settings) {
        nvs_set_u8(h, "units", fahrenheit ? 1 : 0);
        nvs_set_str(h, "place", setting);
        nvs_set_blob(h, "geo", &geo, sizeof(geo));
    }
    if (nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save");
    }
    nvs_close(h);
}

const char *state_name(WeatherState s)
{
    switch (s) {
    case WeatherState::Locating: return "locating";
    case WeatherState::Fetching: return "fetching";
    case WeatherState::Offline: return "offline";
    case WeatherState::Failed: return "failed";
    case WeatherState::NoPlace: return "no_place";
    default: return "idle";
    }
}

// RPC 54 · 63 (iris_ext.h: ESP-Iris's own table is full): none, the report;
// 01 fetch now; 02 then text: the place ("" automatic); 03 then 'c' or 'f':
// the units. The reply is the report (from before a change).
esp_err_t rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t cap, size_t *size, void *)
{
    *size = 0;
    if (request->payload_size >= 1) {
        const uint8_t op = request->payload[0];
        const char *arg = reinterpret_cast<const char *>(request->payload) + 1;
        const size_t len = request->payload_size - 1;
        if (op == 1) {
            weather_refresh();
        } else if (op == 2 && len < 64) {
            char place[64];
            memcpy(place, arg, len);
            place[len] = 0;
            weather_set_place(place);
        } else if (op == 3 && len == 1 && (arg[0] == 'c' || arg[0] == 'f')) {
            weather_set_fahrenheit(arg[0] == 'f');
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    weather_describe(reinterpret_cast<char *>(out), cap);
    *size = strnlen(reinterpret_cast<char *>(out), cap);
    return ESP_OK;
}
}  // namespace

void weather_start()
{
    if (started) {
        return;
    }
    started = true;
    load();
    lv_timer_create(save_cb, 2000, nullptr);
    // Network only (no flash writes): the stack can live in PSRAM.
    xTaskCreatePinnedToCoreWithCaps(worker, "weather", 8192, nullptr, 2, &task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    iris_ext_register(63, rpc, nullptr);
}

bool weather_report(WeatherReport *out)
{
    if (!started) {
        return false;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        *out = S.report;
    }
    // Its age now: "updated" in the clocks' zone, stale past two hours (once
    // the clock is set: before that, there is no telling).
    const time_t now = time(nullptr);
    out->updated[0] = 0;
    if (out->valid && out->fetched > 1600000000) {
        struct tm t {};
        localtime_r(&out->fetched, &t);
        snprintf(out->updated, sizeof(out->updated), "%02d:%02d", t.tm_hour, t.tm_min);
        out->stale = now > 1600000000 && now - out->fetched > STALE_S;
    }
    return true;
}

void weather_refresh()
{
    force = true;
    if (task) {
        xTaskNotifyGive(task);
    }
}

void weather_set_fahrenheit(bool on)
{
    std::lock_guard<std::mutex> g(lock);
    if (S.report.fahrenheit != on) {
        S.report.fahrenheit = on;
        S.save_settings = true;
    }
}

void weather_set_place(const char *place)
{
    char p[64];
    strlcpy(p, place ? place : "", sizeof(p));
    // Spaces round it are not part of it.
    char *start = p;
    while (*start == ' ') {
        ++start;
    }
    for (size_t n = strlen(start); n && start[n - 1] == ' ';) {
        start[--n] = 0;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        if (!strcmp(S.setting, start)) {
            return;
        }
        strlcpy(S.setting, start, sizeof(S.setting));
        // The forecast was for the old place.
        S.report.valid = false;
        S.report.place[0] = 0;
        S.report.automatic = !start[0];
        S.report.state = WeatherState::Fetching;
        S.save_settings = S.save_cache = true;
    }
    weather_refresh();
}

void weather_place(char *out, size_t capacity)
{
    std::lock_guard<std::mutex> g(lock);
    strlcpy(out, S.setting, capacity);
}

const char *weather_config_apply(const char *json, size_t length)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, length) || !doc.is<JsonObject>()) {
        return "expected an object";
    }
    for (JsonPair kv : doc.as<JsonObject>())
        if (strcmp(kv.key().c_str(), "place") && strcmp(kv.key().c_str(), "units")) {
            return "unknown key (use place, units)";
        }
    JsonVariant units = doc["units"];
    if (!units.isNull()) {
        const char *u = units | "";
        if (strcmp(u, "c") && strcmp(u, "f")) {
            return "units: \"c\" or \"f\"";
        }
        weather_set_fahrenheit(!strcmp(u, "f"));
    }
    JsonVariant place = doc["place"];
    if (!place.isNull()) {
        if (!place.is<const char *>() || strlen(place.as<const char *>()) >= 64) {
            return "place: a city or \"latitude,longitude\", under 64 characters; \"\" for automatic";
        }
        weather_set_place(place.as<const char *>());
    }
    return nullptr;  // applies at once
}

void weather_config_status(char *json, size_t capacity)
{
    JsonDocument doc;
    {
        std::lock_guard<std::mutex> g(lock);
        doc["place"] = S.setting;
        doc["units"] = S.report.fahrenheit ? "f" : "c";
        doc["found"] = S.report.place;
    }
    serializeJson(doc, json, capacity);
}

void weather_describe(char *json, size_t capacity)
{
    WeatherReport r;
    weather_report(&r);
    JsonDocument doc;
    doc["state"] = state_name(r.state);
    doc["valid"] = r.valid;
    doc["stale"] = r.stale;
    doc["automatic"] = r.automatic;
    doc["place"] = r.place;
    doc["units"] = r.fahrenheit ? "f" : "c";
    if (r.valid) {
        doc["temp"] = r.temp;
        doc["high"] = r.high;
        doc["low"] = r.low;
        doc["code"] = r.code;
        doc["sky"] = weather_text(r.code);
        doc["day"] = r.day;
        doc["updated"] = r.updated;
        doc["hours"] = r.hour_count;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        doc["setting"] = S.setting;
        doc["error"] = S.error;
    }
    serializeJson(doc, json, capacity);
}
