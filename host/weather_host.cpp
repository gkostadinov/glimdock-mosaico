// SPDX-License-Identifier: Apache-2.0
// The preview's weather (weather.h): a made-up forecast for a made-up place,
// never the network. weather_host_demo picks what the captures show.
#include <string.h>

#include <string>

#include "weather.h"

namespace {
bool fahrenheit = false;
std::string place;  // as set; "" automatic
int demo = 0;
}  // namespace

// 0 the forecast, 1 offline since an older one, 2 none yet (finding where), 3 rain by day.
void weather_host_demo(int which)
{
    demo = which;
}

void weather_start() {}

bool weather_report(WeatherReport *out)
{
    WeatherReport r;
    r.fahrenheit = fahrenheit;
    r.automatic = place.empty();
    strlcpy(r.place, place.empty() ? "Demo City" : place.c_str(), sizeof(r.place));
    if (demo == 2) {
        r.state = WeatherState::Locating;
        r.place[0] = 0;
        *out = r;
        return true;
    }
    r.valid = true;
    r.stale = demo == 1;
    r.temp = demo == 3 ? 12.4f : 17.6f;
    r.high = demo == 3 ? 14.2f : 21.4f;
    r.low = demo == 3 ? 9.1f : 11.8f;
    r.code = demo == 3 ? 63 : 2;
    r.day = demo == 3;  // the demo clock says 20:41: night, but for the rainy day
    strlcpy(r.updated, demo == 1 ? "14:05" : "20:30", sizeof(r.updated));
    static const WeatherHour night[] = {{21, 169, 2, false}, {22, 158, 3, false}, {23, 149, 61, false},
        {0, 142, 61, false}, {1, 136, 3, false}, {2, 131, 0, false}
    };
    static const WeatherHour rain[] = {{13, 128, 63, true}, {14, 131, 80, true}, {15, 135, 3, true},
        {16, 133, 2, true}, {17, 126, 1, true}, {18, 118, 95, true}
    };
    memcpy(r.hours, demo == 3 ? rain : night, sizeof(night));
    r.hour_count = WEATHER_HOURS;
    *out = r;
    return true;
}

void weather_refresh() {}

void weather_set_fahrenheit(bool on)
{
    fahrenheit = on;
}

void weather_set_place(const char *p)
{
    place = p ? p : "";
}

void weather_place(char *out, size_t capacity)
{
    strlcpy(out, place.c_str(), capacity);
}
