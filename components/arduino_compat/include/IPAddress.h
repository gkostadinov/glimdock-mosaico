// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Arduino.h"

// IPv4 address in network byte order, as in the Arduino ESP32 core.
class IPAddress {
public:
    IPAddress() = default;
    explicit IPAddress(uint32_t network_order) : value_(network_order) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        : value_(uint32_t(a) | uint32_t(b) << 8 | uint32_t(c) << 16 | uint32_t(d) << 24) {}
    operator uint32_t() const { return value_; }
    bool fromString(const char *text);
    String toString() const;

private:
    uint32_t value_ = 0;
};
