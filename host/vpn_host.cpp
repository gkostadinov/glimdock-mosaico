// SPDX-License-Identifier: Apache-2.0
// The preview has no network: the VPN screen draws made-up states.
#include <string.h>

#include "../components/vpn_app/src/vpn_ui.h"

namespace {
bool connected = false, auto_connect = true;
}

void vpn_host_demo(bool on)
{
    connected = on;
}

extern "C" void vpn_app_init(const vpn_hooks_t *hooks)
{
    vpn::ui_set_hooks(hooks);
}

extern "C" void vpn_app_summary(vpn_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    out->configured = true;
    out->on = connected;
    out->auto_connect = auto_connect;
    out->full_tunnel = true;
    out->state = connected ? VPN_CONNECTED : VPN_OFF;
    strlcpy(out->endpoint, "vpn.example.com:51820", sizeof(out->endpoint));
    if (connected) {
        strlcpy(out->address, "10.8.0.2", sizeof(out->address));
        out->tunnel_dns = true;
        out->handshake_age_s = 14;
        out->rx_bytes = 19293798;
        out->tx_bytes = 2202009;
    } else {
        out->handshake_age_s = -1;
    }
}

extern "C" void vpn_app_connect(bool on)
{
    connected = on;
}

extern "C" void vpn_app_set_auto_connect(bool on)
{
    auto_connect = on;
}
