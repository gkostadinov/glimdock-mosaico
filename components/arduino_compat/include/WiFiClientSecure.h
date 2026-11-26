// SPDX-License-Identifier: Apache-2.0
// TLS client on esp-tls. setCACert() pins the trust anchor; there is no
// insecure mode. The peer name (host or literal IP) is verified against it.
#pragma once

#include <string>

#include "WiFiClient.h"

struct esp_tls;

// Mirrors the field the Glimdock private reader tunes on the Arduino core.
struct sslclient_context {
    unsigned long handshake_timeout = 120000;  // ms
};

class WiFiClientSecure : public WiFiClient {
public:
    ~WiFiClientSecure() override
    {
        stop();
    }
    void setCACert(const char *pem)
    {
        ca_ = pem ? pem : "";
    }
    // Arduino takes seconds here.
    void setHandshakeTimeout(unsigned long seconds)
    {
        sslclient->handshake_timeout = seconds * 1000UL;
    }
    using WiFiClient::connect;
    int connect(const char *host, uint16_t port, int32_t timeout_ms) override;
    int read(uint8_t *buffer, size_t size) override
    {
        return WiFiClient::read(buffer, size);
    }
    using WiFiClient::read;
    uint8_t connected() override;
    void stop() override;
    bool secure() const override
    {
        return true;
    }

protected:
    bool fill() override;
    int send_some(const uint8_t *data, size_t size) override;

    sslclient_context context_;
    sslclient_context *sslclient = &context_;

private:
    std::string ca_;
    esp_tls *tls_ = nullptr;
};
