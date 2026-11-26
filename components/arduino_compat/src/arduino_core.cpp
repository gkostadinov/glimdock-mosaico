// SPDX-License-Identifier: Apache-2.0
#include "esp_iris.h"
#include "Arduino.h"

#include <fcntl.h>
#include <unistd.h>

#include "IPAddress.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"

HardwareSerial Serial;
EspClass ESP;

uint32_t millis()
{
    return uint32_t(esp_timer_get_time() / 1000);
}

void delay(uint32_t ms)
{
    vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 1);
}

void configTime(long, int, const char *server1, const char *server2, const char *server3)
{
    // Another component (ESP-Iris) may already run SNTP; one client is enough.
    if (esp_sntp_enabled()) {
        return;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server1);
    if (server2) {
        config.num_of_servers = 2;
        config.servers[1] = server2;
    }
    (void)server3;  // two servers are plenty; LWIP_SNTP_MAX_SERVERS may be 1
    config.start = true;
    config.wait_for_sync = false;
    esp_netif_sntp_init(&config);
}

size_t Print::printf(const char *format, ...)
{
    char stack[192];
    va_list args;
    va_start(args, format);
    int needed = vsnprintf(stack, sizeof(stack), format, args);
    va_end(args);
    if (needed < 0) {
        return 0;
    }
    if (size_t(needed) < sizeof(stack)) {
        return write(reinterpret_cast<const uint8_t *>(stack), size_t(needed));
    }
    std::string heap(size_t(needed) + 1, '\0');
    va_start(args, format);
    vsnprintf(heap.data(), heap.size(), format, args);
    va_end(args);
    return write(reinterpret_cast<const uint8_t *>(heap.data()), size_t(needed));
}

namespace {
int serial_peeked = -1;

// On ESP-Mosaico the console belongs to ESP-Iris (USB) and UART0 is not wired
// to the user, so Glimdock's USB line commands have no input: report none.
// Reading stdin here could block the LVGL task.
int serial_poll()
{
    return serial_peeked;
}
}  // namespace

int HardwareSerial::available()
{
    return serial_poll() >= 0 ? 1 : 0;
}
int HardwareSerial::peek()
{
    return serial_poll();
}
int HardwareSerial::read()
{
    int value = serial_poll();
    serial_peeked = -1;
    return value;
}
size_t HardwareSerial::write(uint8_t value)
{
    return write(&value, 1);
}
size_t HardwareSerial::write(const uint8_t *data, size_t size)
{
    size_t written = fwrite(data, 1, size, stdout);
    if (size && data[size - 1] == '\n') {
        fflush(stdout);
    }
    return written;
}

// Through ESP-Iris: on the ESP32-S31 its restart also resets the digital
// peripherals, where a plain esp_restart leaves the USB engine running into
// the next boot (and the board without USB). Glimdock restarts this way after
// its settings change.
void EspClass::restart()
{
    esp_iris_mark_planned_restart();
    esp_iris_restart_now();
    esp_restart();  // only if Iris's restart was already taken
}

bool IPAddress::fromString(const char *text)
{
    if (!text) {
        return false;
    }
    unsigned parts[4];
    int consumed = 0;
    if (sscanf(text, "%3u.%3u.%3u.%3u%n", &parts[0], &parts[1], &parts[2], &parts[3], &consumed) != 4 ||
            text[consumed] != '\0') {
        return false;
    }
    for (unsigned part : parts) {
        if (part > 255) {
            return false;
        }
    }
    *this = IPAddress(uint8_t(parts[0]), uint8_t(parts[1]), uint8_t(parts[2]), uint8_t(parts[3]));
    return true;
}

String IPAddress::toString() const
{
    char text[16];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", unsigned(value_ & 0xff), unsigned((value_ >> 8) & 0xff),
             unsigned((value_ >> 16) & 0xff), unsigned(value_ >> 24));
    return String(text);
}
