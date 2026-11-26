// SPDX-License-Identifier: Apache-2.0
// Captive-portal DNS: answers every A query with one address. Polled.
#pragma once

#include "Arduino.h"
#include "IPAddress.h"

class DNSServer {
public:
    ~DNSServer()
    {
        stop();
    }
    bool start(uint16_t port, const String &domain, IPAddress address);
    void stop();
    void processNextRequest();

private:
    int fd_ = -1;
    IPAddress address_;
};
