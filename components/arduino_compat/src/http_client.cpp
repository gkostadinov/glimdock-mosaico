// SPDX-License-Identifier: Apache-2.0
#include "HTTPClient.h"

#include <strings.h>

#include "WiFiClientSecure.h"

bool HTTPClient::begin(WiFiClient &client, const String &url)
{
    end();
    std::string text(url.c_str());
    size_t scheme_end = text.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    std::string scheme = text.substr(0, scheme_end);
    if (scheme == "http") {
        https_ = false;
        port_ = 80;
    } else if (scheme == "https") {
        https_ = true;
        port_ = 443;
    } else {
        return false;
    }
    size_t authority_start = scheme_end + 3;
    size_t path_start = text.find_first_of("/?#", authority_start);
    std::string authority = text.substr(authority_start, path_start == std::string::npos ? std::string::npos
                                        : path_start - authority_start);
    // Embedded credentials and IPv6 literals are not supported by Glimdock.
    if (authority.empty() || authority.find('@') != std::string::npos || authority[0] == '[') {
        return false;
    }
    size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        char *end = nullptr;
        long port = strtol(authority.c_str() + colon + 1, &end, 10);
        if (!end || *end || port < 1 || port > 65535) {
            return false;
        }
        port_ = uint16_t(port);
        authority.resize(colon);
    }
    host_ = authority;
    path_ = path_start == std::string::npos ? "/" : text.substr(path_start);
    size_t fragment = path_.find('#');
    if (fragment != std::string::npos) {
        path_.resize(fragment);
    }
    if (path_.empty() || path_[0] != '/') {
        path_.insert(path_.begin(), '/');
    }
    client_ = &client;
    size_ = -1;
    return true;
}

void HTTPClient::end()
{
    if (client_) {
        client_->stop();
    }
    client_ = nullptr;
    extra_headers_.clear();
    for (auto &entry : collected_) {
        entry.second.clear();
    }
    size_ = -1;
}

void HTTPClient::addHeader(const String &name, const String &value)
{
    extra_headers_ += name.c_str();
    extra_headers_ += ": ";
    extra_headers_ += value.c_str();
    extra_headers_ += "\r\n";
}

void HTTPClient::collectHeaders(const char *keys[], size_t count)
{
    collected_.clear();
    for (size_t i = 0; i < count; ++i) {
        collected_.emplace_back(keys[i], std::string());
    }
}

String HTTPClient::header(const char *name)
{
    for (auto &entry : collected_) {
        if (!strcasecmp(entry.first.c_str(), name)) {
            return String(entry.second);
        }
    }
    return String();
}

bool HTTPClient::read_line(std::string &line)
{
    line.clear();
    uint32_t began = millis();
    while (millis() - began < timeout_ms_) {
        int value = client_->read();
        if (value < 0) {
            if (!client_->connected()) {
                return false;
            }
            delay(2);
            continue;
        }
        if (value == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }
        if (line.size() >= 2048) {
            return false;  // a header line this long is not a sane response
        }
        line += char(value);
    }
    return false;
}

int HTTPClient::send(const char *method, const uint8_t *payload, size_t size)
{
    if (!client_) {
        return HTTPC_ERROR_NOT_CONNECTED;
    }
    if (https_ != client_->secure()) {
        return HTTPC_ERROR_CONNECTION_REFUSED;
    }
    if (!client_->connect(host_.c_str(), port_, connect_timeout_ms_)) {
        return HTTPC_ERROR_CONNECTION_REFUSED;
    }
    client_->setTimeout(std::max<uint32_t>(1, (timeout_ms_ + 999) / 1000));
    std::string request;
    request.reserve(256 + extra_headers_.size());
    request += method;
    request += ' ';
    request += path_;
    request += http10_ ? " HTTP/1.0\r\nHost: " : " HTTP/1.1\r\nHost: ";
    request += host_;
    if (port_ != (https_ ? 443 : 80)) {
        request += ':' + std::to_string(port_);
    }
    request += "\r\nUser-Agent: ESP32HTTPClient\r\nConnection: close\r\n";
    if (!http10_) {
        request += "Accept-Encoding: identity\r\n";
    }
    request += extra_headers_;
    if (payload || !strcmp(method, "POST")) {
        request += "Content-Length: " + std::to_string(size) + "\r\n";
    }
    request += "\r\n";
    bool sent = client_->write(reinterpret_cast<const uint8_t *>(request.data()), request.size()) == request.size();
    // The request carries bearer tokens; do not leave them in freed heap.
    std::fill(request.begin(), request.end(), '\0');
    if (!sent) {
        return HTTPC_ERROR_SEND_HEADER_FAILED;
    }
    if (size && client_->write(payload, size) != size) {
        return HTTPC_ERROR_SEND_PAYLOAD_FAILED;
    }

    std::string line;
    int code = 0;
    do {  // skip interim 1xx responses
        if (!read_line(line)) {
            return HTTPC_ERROR_READ_TIMEOUT;
        }
        if (line.compare(0, 5, "HTTP/") != 0) {
            return HTTPC_ERROR_NO_HTTP_SERVER;
        }
        size_t space = line.find(' ');
        code = space == std::string::npos ? 0 : atoi(line.c_str() + space + 1);
        if (code <= 0) {
            return HTTPC_ERROR_NO_HTTP_SERVER;
        }
        size_ = -1;
        for (;;) {
            if (!read_line(line)) {
                return HTTPC_ERROR_READ_TIMEOUT;
            }
            if (line.empty()) {
                break;
            }
            size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = line.substr(0, colon);
            size_t value_start = line.find_first_not_of(" \t", colon + 1);
            std::string value = value_start == std::string::npos ? std::string() : line.substr(value_start);
            if (!strcasecmp(name.c_str(), "Content-Length")) {
                char *end = nullptr;
                long length = strtol(value.c_str(), &end, 10);
                size_ = end && !*end && length >= 0 && length <= INT32_MAX ? int(length) : -1;
            }
            for (auto &entry : collected_) {
                if (!strcasecmp(entry.first.c_str(), name.c_str())) {
                    entry.second = value;
                }
            }
        }
    } while (code >= 100 && code < 200);
    return code;
}
