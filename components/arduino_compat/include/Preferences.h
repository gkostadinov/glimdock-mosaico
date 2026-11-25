// SPDX-License-Identifier: Apache-2.0
// Arduino Preferences on ESP-IDF NVS (default partition). Return values follow
// the Arduino core: put* returns the stored size (0 on failure) and string
// getters with a buffer return the length including the terminator.
#pragma once

#include "Arduino.h"
#include "nvs.h"

enum PreferenceType { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB, PT_INVALID };

class Preferences {
public:
    ~Preferences() { end(); }
    bool begin(const char *name, bool read_only = false);
    void end();

    bool isKey(const char *key);
    PreferenceType getType(const char *key);
    bool remove(const char *key);

    uint8_t getUChar(const char *key, uint8_t fallback = 0);
    size_t putUChar(const char *key, uint8_t value);
    bool getBool(const char *key, bool fallback = false) { return getUChar(key, fallback ? 1 : 0) != 0; }
    size_t putBool(const char *key, bool value) { return putUChar(key, value ? 1 : 0); }

    String getString(const char *key, const String &fallback = String());
    size_t getString(const char *key, char *value, size_t capacity);
    size_t putString(const char *key, const char *value);
    size_t putString(const char *key, const String &value) { return putString(key, value.c_str()); }

    size_t getBytesLength(const char *key);
    size_t getBytes(const char *key, void *buffer, size_t capacity);
    size_t putBytes(const char *key, const void *value, size_t length);

private:
    nvs_handle_t handle_ = 0;
    bool open_ = false;
    bool read_only_ = true;
};
