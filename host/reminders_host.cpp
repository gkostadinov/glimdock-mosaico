// SPDX-License-Identifier: Apache-2.0
// The preview's reminders (reminders.h): one, today at 21:30.
#include <string.h>

#include "reminders.h"

bool reminders_next(time_t *at, char *text, size_t capacity)
{
    const time_t now = time(nullptr);
    struct tm t {};
    localtime_r(&now, &t);
    t.tm_hour = 21;
    t.tm_min = 30;
    t.tm_sec = 0;
    *at = mktime(&t);
    strlcpy(text, "Water the plants", capacity);
    return true;
}
