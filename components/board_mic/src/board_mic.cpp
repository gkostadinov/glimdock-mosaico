// SPDX-License-Identifier: Apache-2.0
#include "board_mic.h"

#include <string.h>

#include <atomic>

namespace {
std::atomic<const char *> owner{nullptr};
}

extern "C" bool board_mic_claim(const char *who)
{
    const char *expected = nullptr;
    if (owner.compare_exchange_strong(expected, who)) {
        return true;
    }
    return expected && who && !strcmp(expected, who);
}

extern "C" void board_mic_release(const char *who)
{
    const char *current = owner.load();
    if (current && who && !strcmp(current, who)) {
        owner.compare_exchange_strong(current, nullptr);
    }
}

extern "C" const char *board_mic_owner(void)
{
    return owner.load();
}
