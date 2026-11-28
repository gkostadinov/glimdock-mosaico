// SPDX-License-Identifier: Apache-2.0
// Weather for the home screen's widget: Open-Meteo's forecast (free, no key)
// for the network's location (the time zone's lookup, time_zone.h) or a place
// set in Settings, fetched on a background task every 30 minutes and cached in
// NVS, so the last forecast shows at boot. The host preview links demo data
// instead (host/weather_host.cpp) and never touches the network.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// What the fetching is doing, for a widget without a forecast yet (or an old one).
enum class WeatherState : uint8_t {
    Idle,      // nothing to say: the forecast is fresh
    Locating,  // automatic: waiting for the network's location
    Fetching,
    Offline,   // no network
    Failed,    // the forecast server did not answer as expected
    NoPlace,   // the place set in Settings was not found
};

constexpr int WEATHER_HOURS = 6;
struct WeatherHour {
    int8_t hour;     // local, 0-23
    int16_t temp10;  // degrees Celsius x 10
    uint8_t code;    // WMO weather code
    bool day;
};

struct WeatherReport {
    bool valid = false;  // there is a forecast (fresh, or cached from before)
    bool stale = false;  // it is over two hours old: offline for a while
    WeatherState state = WeatherState::Idle;
    float temp = 0, high = 0, low = 0;  // degrees Celsius: now, today's
    uint8_t code = 0;                   // WMO weather code now
    bool day = true;
    WeatherHour hours[WEATHER_HOURS] = {};  // the next hours
    int hour_count = 0;
    char place[40] = "";   // where, as shown ("Demo City")
    char updated[6] = "";  // the forecast's local time, "HH:MM"
    time_t fetched = 0;
    bool fahrenheit = false;  // the units setting
    bool automatic = true;    // the place comes from the network's location
};

// Loads the cache and the settings and starts the fetching task. On the LVGL
// task (the display lock held): it saves to NVS from an LVGL timer.
void weather_start();
// The latest report (any task). False before weather_start.
bool weather_report(WeatherReport *out);
// Fetches now, when online (a tap on the widget, RPC 54 · 63).
void weather_refresh();
// Units and place (saved). The place: a city ("Lisbon", "Springfield, US") or
// "latitude,longitude" ("38.72,-9.14"); empty or null for automatic.
void weather_set_fahrenheit(bool on);
void weather_set_place(const char *place);
void weather_place(char *out, size_t capacity);  // as set ("" automatic)

// The "weather" section of the board's configuration (RPC 40, web settings):
// {"place": "Lisbon" or "38.72,-9.14" or "" (automatic), "units": "c" or "f"}.
const char *weather_config_apply(const char *json, size_t length);
void weather_config_status(char *json, size_t capacity);
// RPC 54 · 63's reply: the report and what the fetching last did, as JSON.
void weather_describe(char *json, size_t capacity);

// WMO weather codes, as the widget draws and names them.
enum class WeatherSky : uint8_t { Clear, PartlyCloudy, Cloudy, Fog, Drizzle, Rain, Snow, Storm };
inline WeatherSky weather_sky(int code)
{
    if (code <= 1) {
        return WeatherSky::Clear;
    }
    if (code == 2) {
        return WeatherSky::PartlyCloudy;
    }
    if (code == 3) {
        return WeatherSky::Cloudy;
    }
    if (code == 45 || code == 48) {
        return WeatherSky::Fog;
    }
    if (code >= 51 && code <= 57) {
        return WeatherSky::Drizzle;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return WeatherSky::Snow;
    }
    if (code >= 95) {
        return WeatherSky::Storm;
    }
    return code >= 61 ? WeatherSky::Rain : WeatherSky::Cloudy;
}

inline const char *weather_text(int code)
{
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 77: return "Snow";
    case 75: return "Heavy snow";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm, hail";
    default: return "\xE2\x80\x94";
    }
}
