// SPDX-License-Identifier: Apache-2.0
// Arduino HTTPClient subset: one request per begin(), identity bodies only.
// The response body is left in the client for getStreamPtr(); Glimdock reads
// it with its own bounded reader.
#pragma once

#include <string>
#include <vector>

#include "Arduino.h"
#include "WiFiClient.h"

#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_SEND_HEADER_FAILED (-2)
#define HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3)
#define HTTPC_ERROR_NOT_CONNECTED (-4)
#define HTTPC_ERROR_CONNECTION_LOST (-5)
#define HTTPC_ERROR_NO_HTTP_SERVER (-7)
#define HTTPC_ERROR_READ_TIMEOUT (-11)

typedef enum {
    HTTPC_DISABLE_FOLLOW_REDIRECTS,
    HTTPC_STRICT_FOLLOW_REDIRECTS,
    HTTPC_FORCE_FOLLOW_REDIRECTS,
} followRedirects_t;

class HTTPClient {
public:
    ~HTTPClient() { end(); }
    bool begin(WiFiClient &client, const String &url);
    void end();
    void useHTTP10(bool enabled) { http10_ = enabled; }
    void setReuse(bool) {}
    void setConnectTimeout(int32_t ms) { connect_timeout_ms_ = ms; }
    void setTimeout(uint16_t ms) { timeout_ms_ = ms; }
    void setFollowRedirects(followRedirects_t) {}  // redirects are never followed
    void addHeader(const String &name, const String &value);
    void collectHeaders(const char *keys[], size_t count);
    String header(const char *name);
    int GET() { return send("GET", nullptr, 0); }
    int POST(const String &payload) { return send("POST", reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length()); }
    int POST(const uint8_t *payload, size_t size) { return send("POST", payload, size); }
    int getSize() { return size_; }
    WiFiClient *getStreamPtr() { return client_ && client_->connected() ? client_ : nullptr; }

private:
    int send(const char *method, const uint8_t *payload, size_t size);
    bool read_line(std::string &line);

    WiFiClient *client_ = nullptr;
    bool http10_ = false, https_ = false;
    std::string host_, path_;
    uint16_t port_ = 80;
    std::string extra_headers_;
    std::vector<std::pair<std::string, std::string>> collected_;
    int size_ = -1;
    int32_t connect_timeout_ms_ = 5000;
    uint32_t timeout_ms_ = 5000;
};
