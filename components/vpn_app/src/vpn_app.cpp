// SPDX-License-Identifier: Apache-2.0
// The VPN's settings (NVS namespace "vpn"), its configuration section and
// the summary the screen, the launcher and Swifty read.
#include "vpn_app.h"

#include <stdio.h>
#include <string.h>

#include <mutex>
#include <string>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "nvs.h"
#include "vpn_config.h"
#include "vpn_tunnel.h"
#include "vpn_ui.h"

namespace {
const char *TAG = "vpn";
const char *NS = "vpn";

std::mutex lock;
vpn::Settings settings;
vpn::Parsed parsed;
bool on = false, configured = false;
std::string apply_error;  // the last apply's error (apply calls are serialised by the caller)

std::string trimmed(const char *s)
{
    std::string t = s ? s : "";
    const size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
}

void wipe(std::string &s)
{
    volatile char *p = s.data();
    for (size_t i = 0; i < s.size(); ++i) {
        p[i] = 0;
    }
    s.clear();
}

// Under lock.
void refresh()
{
    std::string error;
    configured = vpn::parse(settings, &parsed, &error);
}

void push()
{
    vpn::Settings copy;
    bool want;
    {
        std::lock_guard<std::mutex> g(lock);
        copy = settings;
        want = on;
    }
    vpn::tunnel_set(want, copy);
    copy.wipe();
}

void load()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t size = 0;
    if (nvs_get_str(h, "cfg", nullptr, &size) == ESP_OK && size > 1 && size < 4096) {
        std::string text(size, '\0');
        if (nvs_get_str(h, "cfg", text.data(), &size) == ESP_OK) {
            JsonDocument doc;
            if (!deserializeJson(doc, text.c_str())) {
                settings.private_key = doc["pk"] | "";
                settings.address = doc["addr"] | "";
                settings.dns = doc["dns"] | "";
                settings.public_key = doc["pub"] | "";
                settings.preshared_key = doc["psk"] | "";
                settings.endpoint = doc["ep"] | "";
                settings.allowed_ips = doc["aips"] | "";
                settings.keepalive = doc["ka"] | 0;
                settings.mtu = doc["mtu"] | 0;
                // The document's copies of the keys.
                for (JsonPair kv : doc.as<JsonObject>())
                    if (kv.value().is<const char *>()) {
                        char *v = const_cast<char *>(kv.value().as<const char *>());
                        memset(v, 0, strlen(v));
                    }
            }
        }
        wipe(text);
    }
    uint8_t a = 0;
    if (nvs_get_u8(h, "auto", &a) == ESP_OK) {
        settings.auto_connect = a != 0;
    }
    nvs_close(h);
}

// Any task with an internal-RAM stack (it writes flash).
bool save(const vpn::Settings &s)
{
    std::string text;
    if (!s.empty()) {
        JsonDocument doc;
        doc["pk"] = s.private_key;
        doc["addr"] = s.address;
        doc["dns"] = s.dns;
        doc["pub"] = s.public_key;
        doc["psk"] = s.preshared_key;
        doc["ep"] = s.endpoint;
        doc["aips"] = s.allowed_ips;
        doc["ka"] = s.keepalive;
        if (s.mtu) {
            doc["mtu"] = s.mtu;
        }
        serializeJson(doc, text);
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        wipe(text);
        return false;
    }
    esp_err_t err = text.empty() ? nvs_erase_key(h, "cfg") : nvs_set_str(h, "cfg", text.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "auto", s.auto_connect ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    wipe(text);
    return err == ESP_OK;
}

const char *state_word(vpn_state_t s)
{
    switch (s) {
    case VPN_NOT_SET_UP:
        return "not set up";
    case VPN_WAITING:
        return "waiting for Wi-Fi";
    case VPN_CONNECTING:
        return "connecting";
    case VPN_CONNECTED:
        return "connected";
    case VPN_ERROR:
        return "trouble";
    default:
        return "off";
    }
}

std::string bytes_text(uint64_t n)
{
    char b[24];
    if (n < 1024) {
        snprintf(b, sizeof(b), "%u B", unsigned(n));
    } else if (n < 1024 * 1024) {
        snprintf(b, sizeof(b), "%.1f KB", double(n) / 1024);
    } else if (n < 1024ull * 1024 * 1024) {
        snprintf(b, sizeof(b), "%.1f MB", double(n) / (1024 * 1024));
    } else {
        snprintf(b, sizeof(b), "%.2f GB", double(n) / (1024.0 * 1024 * 1024));
    }
    return b;
}
}  // namespace

extern "C" void vpn_app_init(const vpn_hooks_t *hooks)
{
    vpn::ui_set_hooks(hooks);
    {
        std::lock_guard<std::mutex> g(lock);
        load();
        refresh();
        on = configured && settings.auto_connect;
        ESP_LOGI(TAG, "%s%s", configured ? "set up" : settings.empty() ? "not set up" : "settings incomplete",
                 on ? "; connecting at start-up" : "");
    }
    vpn::tunnel_start();
    vpn::probe_register();
    push();
}

extern "C" void vpn_app_summary(vpn_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    {
        std::lock_guard<std::mutex> g(lock);
        out->configured = configured;
        out->on = on;
        out->auto_connect = settings.auto_connect;
        out->full_tunnel = configured && parsed.full;
        out->mtu = configured ? parsed.mtu : uint16_t(vpn::DEFAULT_MTU);
        strlcpy(out->endpoint, settings.endpoint.c_str(), sizeof(out->endpoint));
    }
    vpn::tunnel_status(out);
}

extern "C" void vpn_app_connect(bool want)
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (on == want || (want && !configured)) {
            return;  // the screen's switch springs back
        }
        on = want;
    }
    ESP_LOGI(TAG, "switched %s", want ? "on" : "off");
    push();
}

extern "C" void vpn_app_set_auto_connect(bool value)
{
    vpn::Settings copy;
    {
        std::lock_guard<std::mutex> g(lock);
        settings.auto_connect = value;
        copy = settings;
    }
    if (!save(copy)) {
        ESP_LOGW(TAG, "auto-connect not saved");
    }
    copy.wipe();
}

extern "C" void vpn_app_report(char *out, size_t capacity)
{
    vpn_summary_t s;
    vpn_app_summary(&s);
    std::string text = std::string("VPN (WireGuard): ") + state_word(s.state);
    if (s.error[0]) {
        text += std::string(": ") + s.error;
    }
    text += ".";
    if (s.state == VPN_CONNECTED || s.state == VPN_CONNECTING || s.state == VPN_ERROR || s.state == VPN_WAITING) {
        text += std::string(" Through ") + s.endpoint + " as " + (s.address[0] ? s.address : "-") + " (" +
                (s.full_tunnel ? "all traffic" : "only the tunnel's network") + ", DNS " +
                (s.tunnel_dns ? "through the tunnel" : "the network's") + ", MTU " + std::to_string(s.mtu) + "); ";
        text += s.handshake_age_s < 0 ? std::string("no handshake yet")
                : "last handshake " + std::to_string(s.handshake_age_s) + " s ago";
        text += "; " + bytes_text(s.rx_bytes) + " in, " + bytes_text(s.tx_bytes) + " out.";
    }
    text += std::string(" Connects at start-up: ") + (s.auto_connect ? "yes." : "no.");
    strlcpy(out, text.c_str(), capacity);
}

extern "C" const char *vpn_app_config_apply(const char *json, size_t length)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, length) || !doc.is<JsonObject>()) {
        return "expected an object";
    }
    static const char *const STRINGS[] = {"private_key", "address", "dns", "public_key", "preshared_key", "endpoint",
                                          "allowed_ips", "conf"
                                         };
    for (JsonPair kv : doc.as<JsonObject>()) {
        const char *k = kv.key().c_str();
        bool known = false;
        for (const char *s : STRINGS) {
            if (!strcmp(k, s)) {
                known = true;
                if (!kv.value().is<const char *>()) {
                    apply_error = std::string(k) + " must be a string";
                    return apply_error.c_str();
                }
            }
        }
        if (!strcmp(k, "persistent_keepalive")) {
            known = true;
            const int v = kv.value().is<int>() ? kv.value().as<int>() : -1;
            if (v < 0 || v > 65535) {
                return "persistent_keepalive: seconds from 0 (off) to 65535";
            }
        } else if (!strcmp(k, "mtu")) {
            known = true;
            const int v = kv.value().is<int>() ? kv.value().as<int>() : -1;
            if (v != 0 && (v < vpn::MIN_MTU || v > vpn::MAX_MTU)) {
                return "mtu: 0 (automatic, 1280) or 576 to 1420";
            }
        } else if (!strcmp(k, "auto_connect") || !strcmp(k, "on")) {
            known = true;
            if (!kv.value().is<bool>()) {
                apply_error = std::string(k) + " must be true or false";
                return apply_error.c_str();
            }
        }
        if (!known) {
            return "unknown key (use conf, private_key, address, dns, public_key, preshared_key, endpoint, allowed_ips, "
                   "persistent_keepalive, mtu, auto_connect, on)";
        }
    }
    vpn::Settings next;
    {
        std::lock_guard<std::mutex> g(lock);
        next = settings;
    }
    bool changed = false;
    if (doc["conf"].is<const char *>()) {
        const std::string conf = doc["conf"].as<const char *>();
        if (trimmed(conf.c_str()).empty()) {
            const bool keep_auto = next.auto_connect;
            next.wipe();
            next = vpn::Settings();
            next.auto_connect = keep_auto;
        } else if (!vpn::parse_conf(conf, &next, &apply_error)) {
            next.wipe();
            return apply_error.c_str();
        }
        changed = true;
    }
    struct Field {
        const char *key;
        std::string vpn::Settings::*field;
    };
    static const Field FIELDS[] = {
        {"private_key", &vpn::Settings::private_key}, {"address", &vpn::Settings::address},
        {"dns", &vpn::Settings::dns},                 {"public_key", &vpn::Settings::public_key},
        {"preshared_key", &vpn::Settings::preshared_key}, {"endpoint", &vpn::Settings::endpoint},
        {"allowed_ips", &vpn::Settings::allowed_ips},
    };
    for (const Field &f : FIELDS)
        if (doc[f.key].is<const char *>()) {
            std::string &dest = next.*(f.field);
            if (f.field == &vpn::Settings::private_key || f.field == &vpn::Settings::preshared_key) {
                wipe(dest);
            }
            dest = trimmed(doc[f.key].as<const char *>());
            changed = true;
        }
    if (!doc["persistent_keepalive"].isNull()) {
        next.keepalive = doc["persistent_keepalive"].as<int>();
        changed = true;
    }
    if (!doc["mtu"].isNull()) {
        next.mtu = doc["mtu"].as<int>();
        changed = true;
    }
    if (!doc["auto_connect"].isNull()) {
        next.auto_connect = doc["auto_connect"].as<bool>();
        changed = true;
    }
    // The key values in the document.
    for (JsonPair kv : doc.as<JsonObject>())
        if (kv.value().is<const char *>()) {
            char *v = const_cast<char *>(kv.value().as<const char *>());
            memset(v, 0, strlen(v));
        }
    vpn::Parsed p;
    std::string why;
    const bool usable = !next.empty() && vpn::parse(next, &p, &why);
    if (changed && !next.empty() && !usable) {
        next.wipe();
        apply_error = why;
        return apply_error.c_str();
    }
    if (doc["on"].as<bool>() && !usable) {
        next.wipe();
        return "on: set up the VPN first (keys, address and endpoint)";
    }
    if (changed && !save(next)) {
        next.wipe();
        return "storage write failed";
    }
    {
        std::lock_guard<std::mutex> g(lock);
        if (changed) {
            settings.wipe();
            settings = next;
            refresh();
        }
        if (!doc["on"].isNull()) {
            on = doc["on"].as<bool>();
        }
        on = on && configured;
    }
    next.wipe();
    ESP_LOGI(TAG, "settings %s%s", changed ? "saved" : "unchanged", doc["on"].isNull() ? "" : on ? "; on" : "; off");
    push();
    return nullptr;  // applies at once
}

extern "C" void vpn_app_config_status(char *json, size_t capacity)
{
    JsonDocument doc;
    {
        std::lock_guard<std::mutex> g(lock);
        doc["configured"] = configured;
        doc["private_key"] = !settings.private_key.empty();
        doc["address"] = settings.address;
        doc["dns"] = settings.dns;
        doc["public_key"] = settings.public_key;
        doc["preshared_key"] = !settings.preshared_key.empty();
        doc["endpoint"] = settings.endpoint;
        doc["allowed_ips"] = settings.allowed_ips;
        doc["persistent_keepalive"] = settings.keepalive;
        doc["mtu"] = settings.mtu;  // 0: automatic
        doc["mtu_in_use"] = configured ? parsed.mtu : vpn::DEFAULT_MTU;
        doc["auto_connect"] = settings.auto_connect;
        doc["on"] = on;
    }
    doc["board_public_key"] = vpn::tunnel_board_key();
    vpn_summary_t s;
    vpn_app_summary(&s);
    doc["state"] = state_word(s.state);
    doc["error"] = s.error;
    doc["tunnel_address"] = s.address;
    doc["handshake_age"] = s.handshake_age_s;
    doc["rx"] = s.rx_bytes;
    doc["tx"] = s.tx_bytes;
    doc["tunnel_dns"] = s.tunnel_dns;
    serializeJson(doc, json, capacity);
}
