// SPDX-License-Identifier: Apache-2.0
// The VPN's settings: the fields of a WireGuard configuration as the person
// gives them, a parser for a whole .conf, and their validation into the
// numbers the tunnel uses. Plain C++ (no lwIP), so the host preview builds it.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace vpn {

// As saved (strings as given, trimmed).
struct Settings {
    std::string private_key, address, dns, public_key, preshared_key, endpoint, allowed_ips;
    int keepalive = 0;  // PersistentKeepalive, seconds (0 off)
    int mtu = 0;        // [Interface] MTU; 0 is DEFAULT_MTU
    bool auto_connect = false;

    bool empty() const
    {
        return private_key.empty() && public_key.empty() && endpoint.empty() && address.empty();
    }
    void wipe();  // overwrite the keys before the memory is let go
};

struct Net4 {
    uint32_t ip = 0;  // host order
    int prefix = 32;
};

// Ready for the tunnel.
struct Parsed {
    Net4 address;              // the tunnel address and its prefix
    uint32_t netmask = 0;      // for the tunnel interface (host order)
    std::vector<Net4> allowed;  // IPv4 AllowedIPs
    std::vector<uint32_t> dns;  // IPv4 DNS servers, at most two
    std::string host;           // the endpoint's name or address
    uint16_t port = 51820;
    uint16_t mtu = 1280;        // the tunnel interface's
    bool full = false;          // 0.0.0.0/0 among AllowedIPs
    bool ignored_ipv6 = false;  // IPv6 entries were left out
};

constexpr int KEY_LEN = 32;
constexpr int MAX_ALLOWED = 6;
// The tunnel's MTU. WireGuard's usual 1420 makes 1480-1500 byte packets once
// encrypted, and paths below 1500 (phone hotspots, often IPv6 with 464XLAT,
// PPPoE) drop them: small requests still work while a TLS handshake (the
// server's certificates) or a page stalls. 1280 puts at most 1360 bytes on
// the wire, and lwIP sizes TCP segments by it both ways (the MSS it announces
// follows the interface's MTU).
constexpr int DEFAULT_MTU = 1280, MIN_MTU = 576, MAX_MTU = 1420;

// Base64 (standard alphabet) to bytes; false unless it decodes to exactly n.
bool base64_decode(const std::string &text, uint8_t *out, size_t n);
std::string base64_encode(const uint8_t *data, size_t n);
bool parse_ip4(const std::string &text, uint32_t *ip);
std::string ip4_text(uint32_t ip);

// Checks a configuration; on failure error says what to fix, in plain words.
bool parse(const Settings &s, Parsed *out, std::string *error);

// Whether the tunnel's DNS servers are used: always with 0.0.0.0/0; in a
// split tunnel only when each is inside the network lwIP routes through it
// (Parsed::netmask), or the lookups would leave over Wi-Fi to a server the
// network cannot reach.
bool dns_through_tunnel(const Parsed &p);

// A WireGuard .conf ([Interface] and the first [Peer]) into s. Keys the
// file does not have are cleared; auto_connect is kept.
bool parse_conf(const std::string &text, Settings *s, std::string *error);

}  // namespace vpn
