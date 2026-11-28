// SPDX-License-Identifier: Apache-2.0
#include "vpn_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace vpn {
namespace {

void wipe_string(std::string &s)
{
    volatile char *p = s.data();
    for (size_t i = 0; i < s.size(); ++i) {
        p[i] = 0;
    }
    s.clear();
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char &c : s) {
        c = char(tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Items of a comma (or space) separated list.
std::vector<std::string> items(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

bool is_ipv6(const std::string &s)
{
    return s.find(':') != std::string::npos;
}

// "10.8.0.2/24" (no prefix: /32).
bool parse_net(const std::string &text, Net4 *out)
{
    const size_t slash = text.find('/');
    if (!parse_ip4(text.substr(0, slash), &out->ip)) {
        return false;
    }
    out->prefix = 32;
    if (slash != std::string::npos) {
        const std::string p = text.substr(slash + 1);
        if (p.empty() || p.size() > 2 || !isdigit(static_cast<unsigned char>(p[0])) ||
                (p.size() == 2 && !isdigit(static_cast<unsigned char>(p[1])))) {
            return false;
        }
        out->prefix = atoi(p.c_str());
        if (out->prefix > 32) {
            return false;
        }
    }
    return true;
}

uint32_t mask_of(int prefix)
{
    return prefix <= 0 ? 0 : prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
}

bool contains(const Net4 &net, uint32_t ip)
{
    return ((net.ip ^ ip) & mask_of(net.prefix)) == 0;
}

int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

bool good_key(const std::string &key)
{
    uint8_t raw[KEY_LEN];
    const bool ok = base64_decode(key, raw, sizeof(raw));
    volatile uint8_t *p = raw;
    for (size_t i = 0; i < sizeof(raw); ++i) {
        p[i] = 0;
    }
    return ok;
}
}  // namespace

void Settings::wipe()
{
    wipe_string(private_key);
    wipe_string(preshared_key);
}

bool base64_decode(const std::string &text, uint8_t *out, size_t n)
{
    size_t len = text.size();
    while (len && text[len - 1] == '=') {
        --len;
    }
    if (text.size() % 4 || text.size() - len > 2) {
        return false;
    }
    size_t got = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        const int v = b64_value(text[i]);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (got >= n) {
                return false;
            }
            out[got++] = uint8_t(acc >> bits);
        }
    }
    return got == n;
}

std::string base64_encode(const uint8_t *data, size_t n)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(data[i]) << 16 | (i + 1 < n ? uint32_t(data[i + 1]) << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
        out += A[v >> 18 & 63];
        out += A[v >> 12 & 63];
        out += i + 1 < n ? A[v >> 6 & 63] : '=';
        out += i + 2 < n ? A[v & 63] : '=';
    }
    return out;
}

bool parse_ip4(const std::string &text, uint32_t *ip)
{
    uint32_t v = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || !isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
        int n = 0, digits = 0;
        while (i < text.size() && isdigit(static_cast<unsigned char>(text[i]))) {
            n = n * 10 + (text[i++] - '0');
            if (++digits > 3 || n > 255) {
                return false;
            }
        }
        v = v << 8 | uint32_t(n);
        if (++parts < 4) {
            if (i >= text.size() || text[i] != '.') {
                return false;
            }
            ++i;
        }
    }
    if (i != text.size()) {
        return false;
    }
    *ip = v;
    return true;
}

std::string ip4_text(uint32_t ip)
{
    char b[16];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", unsigned(ip >> 24), unsigned(ip >> 16 & 255), unsigned(ip >> 8 & 255),
             unsigned(ip & 255));
    return b;
}

bool parse(const Settings &s, Parsed *out, std::string *error)
{
    Parsed p;
    auto fail = [error](const char *why) {
        *error = why;
        return false;
    };
    if (s.private_key.empty()) {
        return fail("No private key: add the board's PrivateKey ([Interface]).");
    }
    if (!good_key(s.private_key)) {
        return fail("The private key is not a WireGuard key (44 characters of base64).");
    }
    // Address: the first IPv4 entry.
    bool have_address = false;
    for (const std::string &a : items(s.address)) {
        if (is_ipv6(a)) {
            p.ignored_ipv6 = true;
        } else if (!have_address) {
            if (!parse_net(a, &p.address) || p.address.ip == 0) {
                return fail("The tunnel address must be an IPv4 address such as 10.8.0.2/24.");
            }
            have_address = true;
        }
    }
    if (!have_address) {
        return fail(s.address.empty() ? "No tunnel address: add Address ([Interface]), such as 10.8.0.2/32."
                    : "The tunnel address must be an IPv4 address such as 10.8.0.2/24.");
    }
    for (const std::string &d : items(s.dns)) {
        uint32_t ip;
        if (is_ipv6(d)) {
            p.ignored_ipv6 = true;
        } else if (parse_ip4(d, &ip)) {
            if (p.dns.size() < 2) {
                p.dns.push_back(ip);
            }
        }
        // Anything else is a search domain: not used.
    }
    if (s.public_key.empty()) {
        return fail("No server key: add the server's PublicKey ([Peer]).");
    }
    if (!good_key(s.public_key)) {
        return fail("The server's public key is not a WireGuard key (44 characters of base64).");
    }
    if (!s.preshared_key.empty() && !good_key(s.preshared_key)) {
        return fail("The preshared key is not a WireGuard key (44 characters of base64).");
    }
    // Endpoint: host:port.
    if (s.endpoint.empty()) {
        return fail("No server address: add Endpoint ([Peer]), such as vpn.example.com:51820.");
    }
    if (s.endpoint[0] == '[') {
        return fail("IPv6 server addresses are not supported yet: use the server's IPv4 address or name.");
    }
    const size_t colon = s.endpoint.rfind(':');
    p.host = s.endpoint.substr(0, colon);
    if (colon != std::string::npos) {
        const std::string port = s.endpoint.substr(colon + 1);
        char *end = nullptr;
        const long v = strtol(port.c_str(), &end, 10);
        if (port.empty() || *end || v < 1 || v > 65535) {
            return fail("The server's port must be a number from 1 to 65535 (Endpoint host:port).");
        }
        p.port = uint16_t(v);
    }
    if (p.host.empty() || p.host.size() > 120) {
        return fail("The server address (Endpoint) is not a name or an IPv4 address.");
    }
    for (char c : p.host) {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.') {
            return fail(c == ':' ? "IPv6 server addresses are not supported yet: use the server's IPv4 address or name."
                        : "The server address (Endpoint) is not a name or an IPv4 address.");
        }
    }
    // AllowedIPs: empty means everything.
    const std::vector<std::string> allowed = items(s.allowed_ips.empty() ? std::string("0.0.0.0/0") : s.allowed_ips);
    for (const std::string &a : allowed) {
        Net4 n;
        if (is_ipv6(a)) {
            p.ignored_ipv6 = true;
            continue;
        }
        if (!parse_net(a, &n)) {
            return fail("AllowedIPs must be IPv4 networks such as 0.0.0.0/0 or 10.8.0.0/24.");
        }
        n.ip &= mask_of(n.prefix);
        if (n.prefix == 0) {
            p.full = true;
        }
        if (int(p.allowed.size()) >= MAX_ALLOWED) {
            return fail("Too many AllowedIPs: the board takes up to 6 networks (or 0.0.0.0/0 for everything).");
        }
        p.allowed.push_back(n);
    }
    if (p.allowed.empty()) {
        return fail("AllowedIPs has no IPv4 network: use 0.0.0.0/0 to send everything through the VPN.");
    }
    if (s.keepalive < 0 || s.keepalive > 65535) {
        return fail("PersistentKeepalive must be from 0 (off) to 65535 seconds.");
    }
    if (s.mtu && (s.mtu < MIN_MTU || s.mtu > MAX_MTU)) {
        return fail("MTU must be from 576 to 1420 (empty: 1280).");
    }
    p.mtu = uint16_t(s.mtu ? s.mtu : DEFAULT_MTU);
    // The tunnel interface's subnet: its own prefix, or the widest allowed
    // network around the address (the part of a split tunnel lwIP can route).
    int prefix = p.address.prefix;
    if (!p.full) {
        for (const Net4 &n : p.allowed)
            if (contains(n, p.address.ip) && n.prefix < prefix) {
                prefix = n.prefix;
            }
    }
    p.netmask = mask_of(prefix);
    *out = p;
    return true;
}

bool dns_through_tunnel(const Parsed &p)
{
    if (p.dns.empty()) {
        return false;
    }
    if (p.full) {
        return true;
    }
    for (uint32_t d : p.dns)
        if ((d & p.netmask) != (p.address.ip & p.netmask)) {
            return false;
        }
    return true;
}

bool parse_conf(const std::string &text, Settings *s, std::string *error)
{
    Settings c;
    c.auto_connect = s->auto_connect;
    enum { NONE, INTERFACE, PEER, OTHER } section = NONE;
    bool have_interface = false, have_peer = false;
    int peers = 0;
    auto add = [](std::string & field, const std::string & v) {
        if (!field.empty()) {
            field += ", ";
        }
        field += v;
    };
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (line[0] == '[') {
            const std::string name = lower(line);
            if (name == "[interface]") {
                section = INTERFACE, have_interface = true;
            } else if (name == "[peer]") {
                section = ++peers == 1 ? PEER : OTHER;  // the first peer only
                have_peer = true;
            } else {
                section = OTHER;
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        // Keys keep their '=' padding: split at the first '=' only.
        const std::string value = trim(line.substr(eq + 1));
        if (section == INTERFACE) {
            if (key == "privatekey") {
                c.private_key = value;
            } else if (key == "address") {
                add(c.address, value);
            } else if (key == "dns") {
                add(c.dns, value);
            } else if (key == "mtu") {
                c.mtu = atoi(value.c_str());
            }
        } else if (section == PEER) {
            if (key == "publickey") {
                c.public_key = value;
            } else if (key == "presharedkey") {
                c.preshared_key = value;
            } else if (key == "endpoint") {
                c.endpoint = value;
            } else if (key == "allowedips") {
                add(c.allowed_ips, value);
            } else if (key == "persistentkeepalive") {
                c.keepalive = lower(value) == "off" ? 0 : atoi(value.c_str());
            }
        }
    }
    if (!have_interface || !have_peer) {
        c.wipe();
        *error = "That is not a WireGuard configuration: it needs an [Interface] and a [Peer] section.";
        return false;
    }
    s->wipe();
    *s = c;
    c.wipe();
    return true;
}

}  // namespace vpn
