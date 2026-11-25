// SPDX-License-Identifier: Apache-2.0
#include "Preferences.h"

#include <vector>

bool Preferences::begin(const char *name, bool read_only)
{
    end();
    if (!name) {
        return false;
    }
    esp_err_t err = nvs_open(name, read_only ? NVS_READONLY : NVS_READWRITE, &handle_);
    open_ = err == ESP_OK;
    read_only_ = read_only;
    return open_;
}

void Preferences::end()
{
    if (!open_) {
        return;
    }
    if (!read_only_) {
        nvs_commit(handle_);
    }
    nvs_close(handle_);
    open_ = false;
}

bool Preferences::isKey(const char *key)
{
    return getType(key) != PT_INVALID;
}

PreferenceType Preferences::getType(const char *key)
{
    nvs_type_t type;
    if (!open_ || !key || nvs_find_key(handle_, key, &type) != ESP_OK) {
        return PT_INVALID;
    }
    switch (type) {
    case NVS_TYPE_U8: return PT_U8;
    case NVS_TYPE_I8: return PT_I8;
    case NVS_TYPE_U16: return PT_U16;
    case NVS_TYPE_I16: return PT_I16;
    case NVS_TYPE_U32: return PT_U32;
    case NVS_TYPE_I32: return PT_I32;
    case NVS_TYPE_U64: return PT_U64;
    case NVS_TYPE_I64: return PT_I64;
    case NVS_TYPE_STR: return PT_STR;
    case NVS_TYPE_BLOB: return PT_BLOB;
    default: return PT_INVALID;
    }
}

bool Preferences::remove(const char *key)
{
    return open_ && !read_only_ && key && nvs_erase_key(handle_, key) == ESP_OK;
}

uint8_t Preferences::getUChar(const char *key, uint8_t fallback)
{
    uint8_t value = fallback;
    if (open_ && key && nvs_get_u8(handle_, key, &value) != ESP_OK) {
        value = fallback;
    }
    return value;
}

size_t Preferences::putUChar(const char *key, uint8_t value)
{
    if (!open_ || read_only_ || !key || nvs_set_u8(handle_, key, value) != ESP_OK) {
        return 0;
    }
    return nvs_commit(handle_) == ESP_OK ? 1 : 0;
}

String Preferences::getString(const char *key, const String &fallback)
{
    size_t length = 0;
    if (!open_ || !key || nvs_get_str(handle_, key, nullptr, &length) != ESP_OK || length == 0) {
        return fallback;
    }
    std::vector<char> buffer(length);
    if (nvs_get_str(handle_, key, buffer.data(), &length) != ESP_OK) {
        return fallback;
    }
    return String(buffer.data());
}

size_t Preferences::getString(const char *key, char *value, size_t capacity)
{
    size_t length = capacity;
    if (!open_ || !key || !value || nvs_get_str(handle_, key, value, &length) != ESP_OK) {
        return 0;
    }
    return length;  // includes the terminator, as in the Arduino core
}

size_t Preferences::putString(const char *key, const char *value)
{
    if (!open_ || read_only_ || !key || !value || nvs_set_str(handle_, key, value) != ESP_OK) {
        return 0;
    }
    return nvs_commit(handle_) == ESP_OK ? strlen(value) : 0;
}

size_t Preferences::getBytesLength(const char *key)
{
    size_t length = 0;
    if (!open_ || !key || nvs_get_blob(handle_, key, nullptr, &length) != ESP_OK) {
        return 0;
    }
    return length;
}

size_t Preferences::getBytes(const char *key, void *buffer, size_t capacity)
{
    size_t length = getBytesLength(key);
    if (!length || !buffer || length > capacity || nvs_get_blob(handle_, key, buffer, &length) != ESP_OK) {
        return 0;
    }
    return length;
}

size_t Preferences::putBytes(const char *key, const void *value, size_t length)
{
    if (!open_ || read_only_ || !key || !value || nvs_set_blob(handle_, key, value, length) != ESP_OK) {
        return 0;
    }
    return nvs_commit(handle_) == ESP_OK ? length : 0;
}
