// SPDX-License-Identifier: Apache-2.0
// Minimal Arduino-core surface used by the Glimdock firmware sources, built on
// ESP-IDF. It is deliberately narrow: only the calls Glimdock makes exist, so a
// new dependency fails at compile time instead of silently misbehaving.
#pragma once

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>

// The Arduino ESP32 core exposes these unqualified, as C99 did.
using std::isfinite;
using std::isinf;
using std::isnan;

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PROGMEM
#define F(value) value

uint32_t millis();
void delay(uint32_t ms);
// Starts SNTP once (UTC); later calls are ignored. Offsets must be zero here.
void configTime(long gmt_offset_s, int daylight_offset_s, const char *server1,
                const char *server2 = nullptr, const char *server3 = nullptr);

template <typename T, typename L, typename H>
constexpr auto constrain(T value, L low, H high)
{
    using V = typename std::common_type<T, L, H>::type;
    return std::max(V(low), std::min(V(value), V(high)));
}
// Mixed-type min/max: on RISC-V int32_t is long, so std::min(int, int32_t)
// would not deduce where the Xtensa build compiled.
template <typename A, typename B>
constexpr typename std::common_type<A, B>::type min(A a, B b)
{
    using V = typename std::common_type<A, B>::type;
    return V(b) < V(a) ? V(b) : V(a);
}
template <typename A, typename B>
constexpr typename std::common_type<A, B>::type max(A a, B b)
{
    using V = typename std::common_type<A, B>::type;
    return V(a) < V(b) ? V(b) : V(a);
}

class String {
public:
    String() = default;
    String(const char *value) : text_(value ? value : "") {}
    String(const char *value, size_t length) : text_(value ? std::string(value, length) : std::string()) {}
    String(char value) : text_(1, value) {}
    String(const std::string &value) : text_(value) {}
    String(std::string &&value) : text_(std::move(value)) {}
    String(double value, unsigned char digits = 2) { format_float(value, digits); }
    String(float value, unsigned char digits = 2) { format_float(value, digits); }
    template <typename T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, char>::value, int>::type = 0>
    String(T value) : text_(std::to_string(value)) {}

    const char *c_str() const { return text_.c_str(); }
    size_t length() const { return text_.size(); }
    bool isEmpty() const { return text_.empty(); }
    void reserve(size_t size) { text_.reserve(size); }
    char operator[](size_t index) const { return index < text_.size() ? text_[index] : '\0'; }
    char charAt(size_t index) const { return (*this)[index]; }

    int indexOf(char value, size_t from = 0) const { return position(text_.find(value, from)); }
    int indexOf(const char *value, size_t from = 0) const { return position(text_.find(value ? value : "", from)); }
    int indexOf(const String &value, size_t from = 0) const { return position(text_.find(value.text_, from)); }
    int lastIndexOf(char value) const { return position(text_.rfind(value)); }
    bool startsWith(const String &prefix) const { return text_.compare(0, prefix.text_.size(), prefix.text_) == 0; }
    bool endsWith(const String &suffix) const
    {
        return suffix.text_.size() <= text_.size() &&
               text_.compare(text_.size() - suffix.text_.size(), suffix.text_.size(), suffix.text_) == 0;
    }
    bool equals(const String &other) const { return text_ == other.text_; }
    bool equalsIgnoreCase(const String &other) const
    {
        if (text_.size() != other.text_.size()) return false;
        for (size_t i = 0; i < text_.size(); ++i) {
            if (tolower((unsigned char)text_[i]) != tolower((unsigned char)other.text_[i])) return false;
        }
        return true;
    }
    String substring(size_t start, size_t end = std::string::npos) const
    {
        start = std::min(start, text_.size());
        end = std::min(end, text_.size());
        return end <= start ? String() : String(text_.substr(start, end - start));
    }
    void trim()
    {
        const auto first = text_.find_first_not_of(" \t\r\n");
        const auto last = text_.find_last_not_of(" \t\r\n");
        text_ = first == std::string::npos ? std::string() : text_.substr(first, last - first + 1);
    }
    void replace(const String &find, const String &replacement)
    {
        if (find.text_.empty()) return;
        size_t at = 0;
        while ((at = text_.find(find.text_, at)) != std::string::npos) {
            text_.replace(at, find.text_.size(), replacement.text_);
            at += replacement.text_.size();
        }
    }
    void toLowerCase() { for (auto &c : text_) c = (char)tolower((unsigned char)c); }
    long toInt() const { return strtol(text_.c_str(), nullptr, 10); }

    bool concat(const char *other) { text_ += other ? other : ""; return true; }  // ArduinoJson writer
    String &operator+=(const String &other) { text_ += other.text_; return *this; }
    String &operator+=(const char *other) { text_ += other ? other : ""; return *this; }
    String &operator+=(char other) { text_ += other; return *this; }
    friend String operator+(String left, const String &right) { return left += right; }
    friend String operator+(String left, const char *right) { return left += right; }
    friend String operator+(const char *left, const String &right) { return String(left) += right; }
    friend bool operator==(const String &left, const String &right) { return left.text_ == right.text_; }
    friend bool operator==(const String &left, const char *right) { return left.text_ == (right ? right : ""); }
    friend bool operator!=(const String &left, const String &right) { return !(left == right); }
    friend bool operator!=(const String &left, const char *right) { return !(left == right); }
    friend bool operator<(const String &left, const String &right) { return left.text_ < right.text_; }

private:
    static int position(size_t at) { return at == std::string::npos ? -1 : int(at); }
    void format_float(double value, unsigned char digits)
    {
        char buffer[48];
        snprintf(buffer, sizeof(buffer), "%.*f", int(digits), value);
        text_ = buffer;
    }
    std::string text_;
};

class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t value) = 0;
    virtual size_t write(const uint8_t *data, size_t size)
    {
        size_t written = 0;
        while (written < size && write(data[written])) ++written;
        return written;
    }
    size_t write(const char *text) { return text ? write(reinterpret_cast<const uint8_t *>(text), strlen(text)) : 0; }
    size_t print(const char *text) { return write(text); }
    size_t print(const String &text) { return write(text.c_str()); }
    size_t println(const char *text = "") { return write(text) + write("\r\n"); }
    size_t println(const String &text) { return println(text.c_str()); }
    size_t printf(const char *format, ...) __attribute__((format(printf, 2, 3)));
};

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual void flush() {}
    void setTimeout(unsigned long ms) { stream_timeout_ms_ = ms; }

protected:
    unsigned long stream_timeout_ms_ = 1000;
};

// Console stream. Output goes to the IDF console; input is a non-blocking read
// of the console UART when one is configured, otherwise always empty.
class HardwareSerial : public Stream {
public:
    void begin(uint32_t) {}
    size_t setRxBufferSize(size_t size) { return size; }
    int available() override;
    int read() override;
    int peek() override;
    size_t write(uint8_t value) override;
    size_t write(const uint8_t *data, size_t size) override;
    using Print::write;
};
extern HardwareSerial Serial;

class EspClass {
public:
    [[noreturn]] void restart();
};
extern EspClass ESP;
