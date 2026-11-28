// SPDX-License-Identifier: Apache-2.0
// The tunnel. One worker task brings it up and down and watches it; lwIP and
// WireGuard state are only touched on lwIP's thread (tcpip_api_call), where
// the WireGuard code itself runs.
//
// Routing while on, with AllowedIPs 0.0.0.0/0 ("full"):
// - the tunnel's interface is lwIP's default, so every destination off the
//   local network goes through it. If the tunnel is down meanwhile, those
//   packets have no route: nothing leaks around it.
// - DNS servers are the configuration's (they are reached through the tunnel);
//   the network's are kept to put back when the tunnel goes.
// - the tunnel's own UDP packets are bound to the Wi-Fi interface (the
//   WireGuard code does that), so the endpoint is reached directly.
// - the local network is still reached directly (lwIP matches an interface's
//   own subnet before the default), as wg-quick leaves it.
// ESP-IDF and DHCP may set the default interface and DNS servers again (a
// Wi-Fi reconnect, a lease renewal): the worker puts them back within a
// second and at once on Wi-Fi events.
// The endpoint's name is looked up with the network's DNS servers over Wi-Fi
// (a small query of our own), so it also works while the tunnel is down and
// still the default route.
#include "vpn_tunnel.h"

#include <string.h>

#include <time.h>

#include <algorithm>
#include <mutex>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"
#include "sodium.h"

extern "C" {
#include "esp_wireguard.h"
#include "wireguard.h"
}
static_assert(WIREGUARD_MAX_SRC_IPS == 8, "built with the WireGuard library's own CONFIG_WIREGUARD_MAX_SRC_IPS");

namespace vpn {
namespace {
const char *TAG = "vpn";

constexpr uint32_t WAKE = 1, WIFI_UP = 2, WIFI_DOWN = 4;
constexpr int64_t S = 1000000;
constexpr int64_t FIRST_ANSWER_US = 20 * S;  // no handshake by then: say so (it keeps trying)
constexpr int64_t RESOLVE_RETRY_US = 10 * S;
constexpr int64_t RERESOLVE_US = 60 * S;  // a name endpoint, lost for this long: look it up again

// Shared with the other tasks (under lock).
struct Shared {
    std::mutex lock;
    bool on = false;
    unsigned generation = 0;  // bumped by every tunnel_set
    Settings settings;
    vpn_state_t state = VPN_OFF;
    std::string error;
    char address[20] = "";
    int32_t handshake_age = -1;
    uint64_t rx = 0, tx = 0;
    bool tunnel_dns = false;
    std::string board_key;
} shared;

// The worker's own (and lwIP's thread's, during its calls).
struct Engine {
    TaskHandle_t task = nullptr;
    unsigned generation = ~0u;
    Settings settings;
    Parsed parsed;
    bool valid = false;
    std::string config_error;
    // The tunnel, while up.
    bool up = false;
    wireguard_config_t wgc = ESP_WIREGUARD_CONFIG_DEFAULT();
    wireguard_ctx_t ctx = ESP_WIREGUARD_CONTEXT_DEFAULT();
    std::string endpoint_ip, address, netmask;
    uint32_t endpoint = 0;  // resolved, host order
    int64_t started_us = 0, last_up_us = 0, next_resolve_us = 0;
    bool answered = false;  // a handshake since the tunnel came up
    // Routing state (lwIP's thread).
    ip_addr_t saved_dns[DNS_MAX_SERVERS] = {};
    bool dns_set = false;
    // Counters (lwIP's thread).
    netif_output_fn wg_output = nullptr;
    udp_recv_fn wg_recv = nullptr;
    void *wg_recv_arg = nullptr;
    uint64_t rx = 0, tx = 0;
    UBaseType_t tcpip_spare = 0;  // words of stack lwIP's thread never used
} E;

// ---- helpers ------------------------------------------------------------------------------------
struct netif *wifi_netif()
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return n ? static_cast<struct netif *>(esp_netif_get_netif_impl(n)) : nullptr;
}

bool wifi_up()
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    return n && esp_netif_is_netif_up(n) && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

void set_ip4(ip_addr_t *a, uint32_t host_order)
{
    ip_addr_set_ip4_u32(a, lwip_htonl(host_order));
}

void publish(vpn_state_t state, const std::string &error)
{
    std::lock_guard<std::mutex> g(shared.lock);
    if (shared.state != state || shared.error != error) {
        ESP_LOGI(TAG, "%s%s%s", state == VPN_OFF ? "off" : state == VPN_NOT_SET_UP ? "not set up" : state == VPN_WAITING ? "waiting for Wi-Fi" :
                 state == VPN_CONNECTING ? "connecting" : state == VPN_CONNECTED ? "connected" : "error", error.empty() ? "" : ": ",
                 error.c_str());
    }
    shared.state = state;
    shared.error = error;
}

// Runs f on lwIP's thread and waits for it.
template <typename F> void on_lwip(F f)
{
    struct Call {
        struct tcpip_api_call_data base;
        F *f;
    } call{};
    call.f = &f;
    tcpip_api_call(
    [](struct tcpip_api_call_data * c) -> err_t {
        (*reinterpret_cast<Call *>(c)->f)();
        return ERR_OK;
    },
    &call.base);
}

// ---- the endpoint's name, over Wi-Fi ------------------------------------------------------------
struct Lookup {
    SemaphoreHandle_t done = nullptr;
    struct udp_pcb *pcb = nullptr;
    uint16_t id = 0;
    uint32_t answer = 0;  // host order; 0 none
    bool failed = false;  // the server answered: no such name
} L;

std::string query(const std::string &host, uint16_t id)
{
    std::string q;
    q += char(id >> 8), q += char(id & 255);
    q += '\x01', q += '\x00';                                        // recursion desired
    q += '\x00', q += '\x01', q.append(6, '\0');                     // one question
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) {
            dot = host.size();
        }
        if (dot > start) {
            q += char(dot - start);
            q.append(host, start, dot - start);
        }
        start = dot + 1;
    }
    q += '\0';
    q += '\x00', q += '\x01', q += '\x00', q += '\x01';  // A, IN
    return q;
}

// lwIP's thread: the first A record of our query's answer.
void lookup_recv(void *, struct udp_pcb *, struct pbuf *p, const ip_addr_t *, u16_t)
{
    uint8_t m[512];
    const uint16_t n = pbuf_copy_partial(p, m, sizeof(m), 0);
    pbuf_free(p);
    if (n < 12 || (m[0] << 8 | m[1]) != L.id || !(m[2] & 0x80)) {
        return;  // not ours
    }
    const int rcode = m[3] & 15, qd = m[4] << 8 | m[5], an = m[6] << 8 | m[7];
    size_t i = 12;
    auto skip_name = [&]() {
        while (i < n) {
            if ((m[i] & 0xC0) == 0xC0) {
                i += 2;
                return;
            }
            if (!m[i]) {
                ++i;
                return;
            }
            i += m[i] + 1;
        }
    };
    for (int k = 0; k < qd && i < n; ++k) {
        skip_name();
        i += 4;
    }
    for (int k = 0; k < an && i + 10 <= n; ++k) {
        skip_name();
        if (i + 10 > n) {
            break;
        }
        const int type = m[i] << 8 | m[i + 1], cls = m[i + 2] << 8 | m[i + 3], len = m[i + 8] << 8 | m[i + 9];
        i += 10;
        if (type == 1 && cls == 1 && len == 4 && i + 4 <= n) {
            L.answer = uint32_t(m[i]) << 24 | uint32_t(m[i + 1]) << 16 | uint32_t(m[i + 2]) << 8 | m[i + 3];
            break;
        }
        i += len;
    }
    L.failed = !L.answer && rcode != 0;
    xSemaphoreGive(L.done);
}

// The endpoint's address (host order), or 0. Uses the network's DNS servers.
uint32_t resolve(const std::string &host, std::string *error)
{
    uint32_t ip;
    if (parse_ip4(host, &ip)) {
        return ip;
    }
    if (!L.done) {
        L.done = xSemaphoreCreateBinary();
    }
    ip_addr_t servers[DNS_MAX_SERVERS] = {};
    struct netif *wifi = nullptr;
    on_lwip([&] {
        wifi = wifi_netif();
        for (int i = 0; i < DNS_MAX_SERVERS; ++i) {
            servers[i] = E.dns_set ? E.saved_dns[i] : *dns_getserver(uint8_t(i));
        }
    });
    if (!wifi) {
        *error = "Wi-Fi is not ready.";
        return 0;
    }
    bool any_server = false, refused = false;
    for (int attempt = 0; attempt < 2; ++attempt)
        for (const ip_addr_t &server : servers) {
            if (!IP_IS_V4(&server) || ip_addr_isany(&server)) {
                continue;
            }
            any_server = true;
            L.id = uint16_t(esp_random());
            L.answer = 0, L.failed = false;
            xSemaphoreTake(L.done, 0);
            const std::string q = query(host, L.id);
            bool sent = false;
            on_lwip([&] {
                L.pcb = udp_new_ip_type(IPADDR_TYPE_V4);
                if (!L.pcb) {
                    return;
                }
                udp_bind_netif(L.pcb, wifi);
                udp_recv(L.pcb, lookup_recv, nullptr);
                struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, uint16_t(q.size()), PBUF_RAM);
                if (p)
                {
                    pbuf_take(p, q.data(), uint16_t(q.size()));
                    sent = udp_sendto_if(L.pcb, p, &server, 53, wifi) == ERR_OK;
                    pbuf_free(p);
                }
            });
            if (sent) {
                xSemaphoreTake(L.done, pdMS_TO_TICKS(2500));
            }
            on_lwip([&] {
                if (L.pcb) {
                    udp_remove(L.pcb);
                    L.pcb = nullptr;
                }
            });
            if (L.answer) {
                return L.answer;
            }
            refused |= L.failed;
        }
    *error = !any_server ? "The network gave no DNS server to find the VPN server with."
             : refused   ? "The VPN server's name (" + host + ") does not exist: check Endpoint."
             : "Could not look up the VPN server's name (" + host + "): no answer from DNS.";
    return 0;
}

// ---- counters (lwIP's thread) -------------------------------------------------------------------
err_t count_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *dest)
{
    const err_t err = E.wg_output(netif, p, dest);
    if (err == ERR_OK) {
        E.tx += 32 + ((p->tot_len + 15u) & ~15u);  // header, padded data, tag: what goes on the wire
    }
    return err;
}

void count_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    E.rx += p->tot_len;
    E.wg_recv(E.wg_recv_arg, pcb, p, addr, port);
}

// ---- routing (lwIP's thread) --------------------------------------------------------------------
bool ours(const ip_addr_t *a)
{
    for (uint32_t d : E.parsed.dns) {
        ip_addr_t x;
        set_ip4(&x, d);
        if (ip_addr_cmp(a, &x)) {
            return true;
        }
    }
    return false;
}

// The tunnel's DNS servers in place (keeping the network's), and with
// 0.0.0.0/0 the tunnel the default. Returns what it had to put back.
const char *claim_routes()
{
    const char *fixed = nullptr;
    if (!E.ctx.netif) {
        return nullptr;
    }
    if (E.parsed.full && netif_default != E.ctx.netif) {
        netif_set_default(E.ctx.netif);
        fixed = "default route";
    }
    // A split tunnel's DNS servers too, when the tunnel reaches them (as
    // wg-quick sets DNS whatever AllowedIPs are): a name that only the home
    // network's DNS knows did not resolve on another network.
    if (dns_through_tunnel(E.parsed)) {
        bool same = true;
        for (int i = 0; i < DNS_MAX_SERVERS; ++i) {
            ip_addr_t want = *IP_ADDR_ANY;
            if (i < int(E.parsed.dns.size())) {
                set_ip4(&want, E.parsed.dns[i]);
            }
            same &= ip_addr_cmp(dns_getserver(uint8_t(i)), &want) != 0;
        }
        if (!same) {
            for (int i = 0; i < DNS_MAX_SERVERS; ++i) {
                const ip_addr_t *cur = dns_getserver(uint8_t(i));
                if (!E.dns_set || (!ours(cur) && !ip_addr_isany(cur))) {
                    E.saved_dns[i] = *cur;  // the network's (DHCP may have changed them)
                }
            }
            for (int i = 0; i < DNS_MAX_SERVERS; ++i) {
                ip_addr_t want = *IP_ADDR_ANY;
                if (i < int(E.parsed.dns.size())) {
                    set_ip4(&want, E.parsed.dns[i]);
                }
                dns_setserver(uint8_t(i), &want);
            }
            E.dns_set = true;
            fixed = fixed ? "default route and DNS" : "DNS";
        }
    }
    return fixed;
}

void release_routes()
{
    if (E.dns_set) {
        for (int i = 0; i < DNS_MAX_SERVERS; ++i) {
            dns_setserver(uint8_t(i), &E.saved_dns[i]);
        }
        E.dns_set = false;
    }
    if (E.ctx.netif && netif_default == E.ctx.netif) {
        struct netif *wifi = wifi_netif();
        netif_set_default(wifi ? wifi : E.ctx.netif_default);
    }
}

// ---- up and down (worker) -----------------------------------------------------------------------
bool bring_up(std::string *error)
{
#if !LWIP_ESP_NETIF_DATA
    // ESP-NETIF would read the tunnel's netif->state as its own (see
    // CONFIG_ESP_NETIF_BRIDGE_EN in sdkconfig.defaults) and crash.
    *error = "This firmware was built without CONFIG_ESP_NETIF_BRIDGE_EN (sdkconfig.defaults), so the VPN cannot start.";
    return false;
#endif
    E.endpoint_ip = ip4_text(E.endpoint);
    E.address = ip4_text(E.parsed.address.ip);
    E.netmask = ip4_text(E.parsed.netmask);
    E.wgc = ESP_WIREGUARD_CONFIG_DEFAULT();
    E.wgc.private_key = E.settings.private_key.c_str();
    E.wgc.public_key = E.settings.public_key.c_str();
    E.wgc.preshared_key = E.settings.preshared_key.empty() ? nullptr : E.settings.preshared_key.c_str();
    E.wgc.address = E.address.c_str();
    E.wgc.netmask = E.netmask.c_str();
    E.wgc.endpoint = E.endpoint_ip.c_str();
    E.wgc.port = E.parsed.port;
    E.wgc.persistent_keepalive = uint16_t(E.settings.keepalive);
    E.ctx = ESP_WIREGUARD_CONTEXT_DEFAULT();
    const char *failed = nullptr;
    esp_err_t err = ESP_OK;
    on_lwip([&] {
        E.rx = E.tx = 0;
        E.dns_set = false;
        err = esp_wireguard_init(&E.wgc, &E.ctx);
        if (err != ESP_OK) {
            failed = "init";
            return;
        }
        err = esp_wireguard_connect(&E.ctx);
        if (err != ESP_OK)
        {
            failed = "connect";
            if (E.ctx.netif) {
                esp_wireguard_disconnect(&E.ctx);
            }
            E.ctx.netif = nullptr;
            return;
        }
        for (const Net4 &n : E.parsed.allowed)
        {
            const std::string ip = ip4_text(n.ip), mask = ip4_text(n.prefix ? ~((1u << (32 - n.prefix)) - 1) : 0);
            if (esp_wireguard_add_allowed_ip(&E.ctx, ip.c_str(), mask.c_str()) != ESP_OK) {
                ESP_LOGW(TAG, "allowed network %s/%d not added", ip.c_str(), n.prefix);
            }
        }
        struct netif *wg = E.ctx.netif;
        // The library makes the interface 1420 bytes (WireGuard's default); a
        // smaller one keeps the encrypted packets under the path's MTU (see
        // DEFAULT_MTU). TCP reads it when a connection opens, before any goes
        // through the tunnel.
        wg->mtu = E.parsed.mtu;
        // Count what goes through.
        auto *device = static_cast<struct wireguard_device *>(wg->state);
        E.wg_output = wg->output;
        wg->output = count_output;
        if (device && device->udp_pcb)
        {
            E.wg_recv = device->udp_pcb->recv;
            E.wg_recv_arg = device->udp_pcb->recv_arg;
            udp_recv(device->udp_pcb, count_recv, nullptr);
        }
        claim_routes();
    });
    if (failed) {
        ESP_LOGW(TAG, "WireGuard %s: %s", failed, esp_err_to_name(err));
        *error = err == ESP_ERR_NO_MEM || err == ESP_FAIL ? "WireGuard could not start (keys, or out of memory)."
                 : "WireGuard could not start: " + std::string(esp_err_to_name(err)) + ".";
        return false;
    }
    E.up = true;
    E.answered = false;
    E.started_us = esp_timer_get_time();
    E.last_up_us = 0;
    ESP_LOGI(TAG, "tunnel up as %s/%d to port %u (%s, DNS %s, MTU %u)", E.address.c_str(), E.parsed.address.prefix,
             E.parsed.port, E.parsed.full ? "all traffic" : "split",
             dns_through_tunnel(E.parsed) ? "the tunnel's" : E.parsed.dns.empty() ? "the network's" : "the network's (the tunnel's are outside its networks)",
             unsigned(E.parsed.mtu));
    return true;
}

void bring_down()
{
    if (!E.up) {
        return;
    }
    on_lwip([] {
        release_routes();
        if (E.ctx.netif) {
            esp_wireguard_disconnect(&E.ctx);
        }
        E.ctx.netif = nullptr;
    });
    E.up = false;
    ESP_LOGI(TAG, "tunnel down");
}

// ---- the worker ---------------------------------------------------------------------------------
void step(uint32_t events)
{
    bool on;
    {
        std::lock_guard<std::mutex> g(shared.lock);
        on = shared.on;
        if (shared.generation != E.generation) {
            E.generation = shared.generation;
            const bool changed = E.settings.private_key != shared.settings.private_key ||
                                 E.settings.public_key != shared.settings.public_key ||
                                 E.settings.preshared_key != shared.settings.preshared_key ||
                                 E.settings.address != shared.settings.address || E.settings.dns != shared.settings.dns ||
                                 E.settings.endpoint != shared.settings.endpoint ||
                                 E.settings.allowed_ips != shared.settings.allowed_ips ||
                                 E.settings.keepalive != shared.settings.keepalive ||
                                 E.settings.mtu != shared.settings.mtu;
            if (changed) {
                if (E.up) {
                    shared.lock.unlock();
                    bring_down();
                    shared.lock.lock();
                }
                const bool new_key = E.settings.private_key != shared.settings.private_key || E.generation == 1;
                E.settings.wipe();
                E.settings = shared.settings;
                E.valid = parse(E.settings, &E.parsed, &E.config_error);
                if (new_key) {
                    shared.board_key = public_key_of(E.settings.private_key);
                }
                E.next_resolve_us = 0;
                E.endpoint = 0;
            }
        }
    }
    if (!on) {
        bring_down();
        {
            std::lock_guard<std::mutex> g(shared.lock);
            shared.handshake_age = -1;
            shared.tunnel_dns = false;
        }
        publish(VPN_OFF, "");
        return;
    }
    if (!E.valid) {
        publish(VPN_NOT_SET_UP, E.settings.empty() ? "No VPN set up yet: add a WireGuard configuration in Web settings."
                : E.config_error);
        return;
    }
    const int64_t now = esp_timer_get_time();
    const bool wifi = wifi_up();
    if (!E.up) {
        if (!wifi) {
            publish(VPN_WAITING, "");
            return;
        }
        if (now < E.next_resolve_us) {
            return;  // the last attempt's error stays shown
        }
        std::string error;
        E.endpoint = resolve(E.parsed.host, &error);
        if (!E.endpoint) {
            E.next_resolve_us = now + RESOLVE_RETRY_US;
            publish(VPN_ERROR, error);
            return;
        }
        if (!bring_up(&error)) {
            E.next_resolve_us = now + RESOLVE_RETRY_US;
            publish(VPN_ERROR, error);
            return;
        }
        E.next_resolve_us = now + RERESOLVE_US;
        {
            std::lock_guard<std::mutex> g(shared.lock);
            strlcpy(shared.address, E.address.c_str(), sizeof(shared.address));
            shared.tunnel_dns = dns_through_tunnel(E.parsed);
        }
    }
    // Up: watch it.
    bool peer_up = false;
    time_t handshake = 0;
    const char *fixed = nullptr;
    uint64_t rx = 0, tx = 0;
    on_lwip([&] {
        peer_up = esp_wireguard_peer_is_up(&E.ctx) == ESP_OK;
        if (esp_wireguard_latest_handshake(&E.ctx, &handshake) != ESP_OK) {
            handshake = 0;
        }
        if (wifi)
        {
            fixed = claim_routes();
        }
        rx = E.rx, tx = E.tx;
        E.tcpip_spare = uxTaskGetStackHighWaterMark(nullptr);
    });
    if (fixed) {
        ESP_LOGI(TAG, "put back the tunnel's %s", fixed);
    }
    if (peer_up) {
        E.answered = true;
        E.last_up_us = now;
    }
    {
        std::lock_guard<std::mutex> g(shared.lock);
        shared.handshake_age = handshake ? int32_t(std::max<time_t>(0, time(nullptr) - handshake)) : -1;
        shared.rx = rx, shared.tx = tx;
    }
    // A name endpoint whose address may have changed: look again after a
    // Wi-Fi reconnect, or while no answer comes.
    const bool lost = !peer_up && now - (E.last_up_us ? E.last_up_us : E.started_us) > RERESOLVE_US;
    if (wifi && E.parsed.host != E.endpoint_ip && ((events & WIFI_UP) || (lost && now >= E.next_resolve_us))) {
        std::string error;
        const uint32_t ip = resolve(E.parsed.host, &error);
        E.next_resolve_us = now + RERESOLVE_US;
        if (ip && ip != E.endpoint) {
            ESP_LOGI(TAG, "the server's address changed: reconnecting");
            bring_down();
            E.endpoint = ip;
            if (!bring_up(&error)) {
                publish(VPN_ERROR, error);
                return;
            }
        }
    }
    if (!wifi) {
        publish(VPN_WAITING, "");
    } else if (peer_up) {
        publish(VPN_CONNECTED, "");
    } else if (!E.answered && now - E.started_us > FIRST_ANSWER_US) {
        publish(VPN_ERROR,
                "The VPN server does not answer. Check Endpoint and its port, and that the server has this board's public key.");
    } else if (E.answered && now - E.last_up_us > 3 * 60 * S) {
        publish(VPN_ERROR, "Lost the VPN server: no answer for " + std::to_string(int((now - E.last_up_us) / (60 * S))) +
                " min. Still trying.");
    } else {
        publish(VPN_CONNECTING, "");
    }
}

void worker(void *)
{
    for (;;) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, pdMS_TO_TICKS(1000));
        step(events);
    }
}

void on_wifi(void *, esp_event_base_t base, int32_t id, void *)
{
    if (!E.task) {
        return;
    }
    const bool up = base == IP_EVENT && id == IP_EVENT_STA_GOT_IP;
    xTaskNotify(E.task, up ? WIFI_UP : WIFI_DOWN, eSetBits);
}
}  // namespace

void tunnel_start()
{
    if (E.task) {
        return;
    }
    // Below the LVGL draw task (3); it mostly waits on lwIP's thread. Its stack
    // can live in PSRAM: it never writes flash.
    xTaskCreatePinnedToCoreWithCaps(worker, "vpn", 6144, nullptr, 2, &E.task, tskNO_AFFINITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, nullptr);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, nullptr);
}

void tunnel_set(bool on, const Settings &settings)
{
    {
        std::lock_guard<std::mutex> g(shared.lock);
        shared.on = on;
        shared.settings.wipe();
        shared.settings = settings;
        ++shared.generation;
    }
    if (E.task) {
        xTaskNotify(E.task, WAKE, eSetBits);
    }
}

void tunnel_status(vpn_summary_t *out)
{
    std::lock_guard<std::mutex> g(shared.lock);
    out->state = shared.state;
    strlcpy(out->error, shared.error.c_str(), sizeof(out->error));
    const bool live = shared.state == VPN_CONNECTED || shared.state == VPN_CONNECTING || shared.state == VPN_ERROR ||
                      shared.state == VPN_WAITING;
    strlcpy(out->address, live ? shared.address : "", sizeof(out->address));
    out->handshake_age_s = live ? shared.handshake_age : -1;
    out->rx_bytes = shared.rx;
    out->tx_bytes = shared.tx;
    out->tunnel_dns = live && shared.tunnel_dns;
}

std::string public_key_of(const std::string &private_key)
{
    uint8_t priv[KEY_LEN], pub[KEY_LEN];
    std::string out;
    if (base64_decode(private_key, priv, sizeof(priv)) && crypto_scalarmult_curve25519_base(pub, priv) == 0) {
        out = base64_encode(pub, sizeof(pub));
    }
    sodium_memzero(priv, sizeof(priv));
    return out;
}

std::string tunnel_board_key()
{
    std::lock_guard<std::mutex> g(shared.lock);
    return shared.board_key;
}

void tunnel_route(char *out, size_t capacity)
{
    std::string text;
    on_lwip([&] {
        char ip[IP4ADDR_STRLEN_MAX];
        if (netif_default) {
            text = std::string("default ") + netif_default->name[0] + netif_default->name[1] + std::to_string(netif_default->num) +
            " " + ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip, sizeof(ip));
        } else {
            text = "no default route";
        }
        text += "; DNS";
        for (int i = 0; i < DNS_MAX_SERVERS; ++i)
        {
            const ip_addr_t *d = dns_getserver(uint8_t(i));
            if (!ip_addr_isany(d) && IP_IS_V4(d)) {
                text += std::string(" ") + ip4addr_ntoa_r(ip_2_ip4(d), ip, sizeof(ip));
            }
        }
    });
    strlcpy(out, text.c_str(), capacity);
}

void tunnel_debug(char *out, size_t capacity)
{
    snprintf(out, capacity, "tunnel %s; lwIP thread stack spare %u bytes", E.up ? "up" : "down",
             unsigned(E.tcpip_spare * sizeof(StackType_t)));
}

}  // namespace vpn
