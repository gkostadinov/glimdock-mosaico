// SPDX-License-Identifier: Apache-2.0
// Arduino-style TCP client on lwIP sockets. Reads are buffered and never block:
// available() pulls what has arrived, so callers can impose their own deadlines.
#pragma once

#include "Arduino.h"
#include "IPAddress.h"

class WiFiClient : public Stream {
public:
    WiFiClient() = default;
    // Receive buffer lives on the heap (PSRAM when available) only while
    // connected: Glimdock places clients on deep task stacks.
    static constexpr size_t RX_CAPACITY = 1024;
    WiFiClient(const WiFiClient &) = delete;
    WiFiClient &operator=(const WiFiClient &) = delete;
    ~WiFiClient() override
    {
        stop();
    }

    int connect(IPAddress address, uint16_t port)
    {
        return connect(address.toString().c_str(), port, int32_t(_timeout));
    }
    int connect(const char *host, uint16_t port)
    {
        return connect(host, port, int32_t(_timeout));
    }
    virtual int connect(const char *host, uint16_t port, int32_t timeout_ms);

    size_t write(uint8_t value) override
    {
        return write(&value, 1);
    }
    size_t write(const uint8_t *data, size_t size) override;
    using Print::write;
    int available() override;
    int read() override;
    virtual int read(uint8_t *buffer, size_t size);
    int peek() override;
    virtual uint8_t connected();
    virtual void stop();
    // Arduino WiFiClient::setTimeout takes seconds; it bounds blocking writes.
    void setTimeout(uint32_t seconds)
    {
        io_timeout_ms_ = seconds * 1000U;
    }
    explicit operator bool()
    {
        return connected();
    }
    virtual bool secure() const
    {
        return false;
    }

protected:
    // Move newly arrived bytes into the receive buffer without blocking.
    // Returns false once the peer closed or the connection failed.
    virtual bool fill();
    virtual int send_some(const uint8_t *data, size_t size);
    bool wait_writable(uint32_t timeout_ms);

    int fd_ = -1;
    bool eof_ = false;
    uint8_t *rx_ = nullptr;
    bool ensure_rx();
    size_t rx_len_ = 0, rx_pos_ = 0;
    unsigned long _timeout = 3000;  // connect timeout in ms, as in the Arduino core
    uint32_t io_timeout_ms_ = 5000;
};
