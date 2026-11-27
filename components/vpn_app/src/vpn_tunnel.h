// SPDX-License-Identifier: Apache-2.0
// The WireGuard tunnel (esphome/wireguard on lwIP) and the routing around it,
// run by one worker task. lwIP is only touched on its own thread
// (tcpip_api_call).
#pragma once

#include "vpn_app.h"
#include "vpn_config.h"

namespace vpn {

void tunnel_start();
// The person's wish and the settings to use (copied).
void tunnel_set(bool on, const Settings &settings);
// The live part of the summary: state, error, address, handshake, bytes.
void tunnel_status(vpn_summary_t *out);
// The board's public key from its private key (base64), or "" when invalid.
// X25519 wants a roomy stack: the worker works it out for each new private
// key, and tunnel_board_key() returns the latest.
std::string public_key_of(const std::string &private_key);
std::string tunnel_board_key();
// Diagnostics for the logs and Swifty: lwIP's thread stack to spare, etc.
void tunnel_debug(char *out, size_t capacity);
// The route and DNS servers in use: "default wg0 10.8.0.2; DNS 10.8.0.1".
void tunnel_route(char *out, size_t capacity);
// ESP-Iris RPC 52 (vpn_probe.cpp): fetch a URL, to see where traffic goes.
void probe_register();

}  // namespace vpn
