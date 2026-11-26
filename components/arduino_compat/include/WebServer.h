// SPDX-License-Identifier: Apache-2.0
// Minimal polled HTTP/1.0 server for Glimdock's local setup portal. One client
// is served per handleClient() call; request bodies are capped at 8 KiB.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Arduino.h"

enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };

class WebServer {
public:
    using Handler = std::function<void()>;
    explicit WebServer(int port = 80) : port_(port) {}
    ~WebServer()
    {
        stop();
    }
    void begin();
    void stop();
    void handleClient();
    void on(const String &uri, HTTPMethod method, Handler handler);
    void onNotFound(Handler handler)
    {
        not_found_ = std::move(handler);
    }
    String arg(const String &name) const;
    bool hasArg(const String &name) const;
    void sendHeader(const String &name, const String &value, bool first = false);
    void send(int code, const char *content_type, const String &body);

private:
    struct Route {
        std::string uri;
        HTTPMethod method;
        Handler handler;
    };
    void parse_arguments(const std::string &encoded);

    int port_;
    int listen_fd_ = -1, client_fd_ = -1;
    std::vector<Route> routes_;
    Handler not_found_;
    std::vector<std::pair<std::string, std::string>> args_;
    std::string response_headers_;
    bool responded_ = false;
};
