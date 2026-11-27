// SPDX-License-Identifier: Apache-2.0
// Web settings (see web_settings.h): a small HTTP server that runs only while
// Settings' switch is on. Routes:
//   GET  /             the page (web_settings.html)
//   POST /api/login    body: the code on the board; sets the session cookie
//   GET  /api/status   the configuration, as tools/mosaico-config.py --status
//   POST /api/save     a change in the JSON tools/mosaico-config.py sends
//   POST /api/stop     switches the web settings off
// Every POST needs the header "X-Mosaico: 1" (a page on another site cannot
// send it without asking first), and all but the login the session cookie.
#include "web_settings.h"

#include <stdio.h>
#include <string.h>

#include <mutex>
#include <string>

#include "ArduinoJson.h"
#include "WiFi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/platform_util.h"
#include "swifty_app.h"

extern const char page_start[] asm("_binary_web_settings_html_start");
extern const char page_end[] asm("_binary_web_settings_html_end");

namespace {
const char *TAG = "web-settings";
constexpr int64_t IDLE_US = 10LL * 60 * 1000000;  // off after 10 minutes without a request
constexpr int MAX_WRONG = 5;                      // off after this many wrong codes
constexpr size_t MAX_BODY = 16 * 1024;
constexpr int64_t RESTART_DELAY_US = 1500000;     // time for the reply to reach the browser

std::mutex lock;
struct {
    httpd_handle_t server = nullptr;
    char url[40] = "", code[8] = "", note[64] = "";
    char sessions[4][33] = {};  // signed-in browsers' cookies (hex), the oldest replaced first
    int next_session = 0;
    int wrong = 0;
    int64_t last = 0;  // the last request
    esp_timer_handle_t idle = nullptr, stop_soon = nullptr, restart_soon = nullptr;
    char stop_note[64] = "";
    void (*restart)() = nullptr;
} W;

bool same(const char *a, const char *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) {
        d |= uint8_t(a[i] ^ b[i]);
    }
    return d == 0;
}

// Stops from outside the server's own task (it cannot stop itself).
void stop_later(const char *note)
{
    {
        std::lock_guard<std::mutex> g(lock);
        strlcpy(W.stop_note, note, sizeof(W.stop_note));
    }
    esp_timer_stop(W.stop_soon);
    esp_timer_start_once(W.stop_soon, 50000);
}

esp_err_t send_json(httpd_req_t *r, const char *status, const char *key, const char *value)
{
    JsonDocument doc;
    doc[key] = value;
    std::string body;
    serializeJson(doc, body);
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, body.data(), ssize_t(body.size()));
}

// A POST from the page itself (see the top), and the activity that keeps it on.
bool from_page(httpd_req_t *r)
{
    char v[4] = "";
    return httpd_req_get_hdr_value_str(r, "X-Mosaico", v, sizeof(v)) == ESP_OK && !strcmp(v, "1");
}

bool signed_in(httpd_req_t *r)
{
    char cookie[40] = "";
    size_t n = sizeof(cookie);
    const bool has = httpd_req_get_cookie_val(r, "mosaico", cookie, &n) == ESP_OK;
    std::lock_guard<std::mutex> g(lock);
    W.last = esp_timer_get_time();
    bool ok = false;
    for (const char *s : W.sessions) {
        ok |= has && s[0] && strlen(cookie) == 32 && same(cookie, s, 32);
    }
    return ok;
}

bool read_body(httpd_req_t *r, std::string &out)
{
    if (r->content_len > MAX_BODY) {
        return false;
    }
    out.resize(r->content_len);
    size_t got = 0;
    while (got < out.size()) {
        const int n = httpd_req_recv(r, out.data() + got, out.size() - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            mbedtls_platform_zeroize(out.data(), out.size());
            return false;
        }
        got += size_t(n);
    }
    return true;
}

esp_err_t page(httpd_req_t *r)
{
    {
        std::lock_guard<std::mutex> g(lock);
        W.last = esp_timer_get_time();
    }
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    // The embedded file ends in a NUL (EMBED_TXTFILES).
    return httpd_resp_send(r, page_start, ssize_t(page_end - page_start - 1));
}

esp_err_t login(httpd_req_t *r)
{
    std::string code;
    if (!from_page(r) || !read_body(r, code)) {
        return send_json(r, "400 Bad Request", "error", "bad request");
    }
    while (!code.empty() && (code.back() == ' ' || code.back() == '\n' || code.back() == '\r')) {
        code.pop_back();
    }
    char cookie[96] = "";
    int wrong = 0;
    {
        std::lock_guard<std::mutex> g(lock);
        W.last = esp_timer_get_time();
        // Once locked out, not even the right code (the server is stopping).
        if (W.wrong < MAX_WRONG && code.size() == 6 && same(code.c_str(), W.code, 6)) {
            uint8_t raw[16];
            esp_fill_random(raw, sizeof(raw));
            char *session = W.sessions[W.next_session];
            W.next_session = (W.next_session + 1) % 4;
            for (int i = 0; i < 16; ++i) {
                snprintf(session + i * 2, 3, "%02x", raw[i]);
            }
            mbedtls_platform_zeroize(raw, sizeof(raw));
            snprintf(cookie, sizeof(cookie), "mosaico=%s; Path=/; HttpOnly; SameSite=Strict", session);
        } else {
            wrong = ++W.wrong;
        }
    }
    if (!cookie[0]) {
        ESP_LOGW(TAG, "a wrong code (%d of %d)", wrong, MAX_WRONG);
        if (wrong >= MAX_WRONG) {
            if (wrong == MAX_WRONG) {
                stop_later("Turned off after 5 wrong codes");
            }
            return send_json(r, "403 Forbidden", "error", "Too many wrong codes: web settings are off.");
        }
        return send_json(r, "403 Forbidden", "error", "That is not the code on the board.");
    }
    ESP_LOGI(TAG, "a browser signed in");
    httpd_resp_set_hdr(r, "Set-Cookie", cookie);
    const esp_err_t err = send_json(r, "200 OK", "reply", "ok");
    mbedtls_platform_zeroize(cookie, sizeof(cookie));
    return err;
}

esp_err_t status(httpd_req_t *r)
{
    if (!signed_in(r)) {
        return send_json(r, "401 Unauthorized", "error", "sign in");
    }
    char *json = swifty_app_config_status();
    if (!json) {
        return send_json(r, "500 Internal Server Error", "error", "out of memory");
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(r, json);
    free(json);
    return err;
}

esp_err_t save(httpd_req_t *r)
{
    if (!from_page(r)) {
        return send_json(r, "400 Bad Request", "error", "bad request");
    }
    if (!signed_in(r)) {
        return send_json(r, "401 Unauthorized", "error", "sign in");
    }
    std::string body;
    if (!read_body(r, body)) {
        return send_json(r, "413 Payload Too Large", "error", "too large");
    }
    char reply[160];
    swifty_app_config_apply(body.data(), body.size(), reply, sizeof(reply));  // wipes body
    ESP_LOGI(TAG, "saved from the browser: %s", reply);
    if (!strcmp(reply, "ok restart") && W.restart) {
        esp_timer_start_once(W.restart_soon, RESTART_DELAY_US);
    }
    return send_json(r, "200 OK", "reply", reply);
}

esp_err_t stop(httpd_req_t *r)
{
    if (!from_page(r) || !signed_in(r)) {
        return send_json(r, "401 Unauthorized", "error", "sign in");
    }
    stop_later("Turned off from the browser");
    return send_json(r, "200 OK", "reply", "ok");
}

void idle_check(void *)
{
    bool idle;
    {
        std::lock_guard<std::mutex> g(lock);
        idle = W.server && esp_timer_get_time() - W.last > IDLE_US;
    }
    if (idle) {
        web_settings_stop("Turned off after 10 minutes unused");
    }
}

void make_timers()
{
    if (W.idle) {
        return;
    }
    esp_timer_create_args_t a = {};
    a.callback = idle_check;
    a.name = "web-idle";
    esp_timer_create(&a, &W.idle);
    a.callback = [](void *) {
        char note[64];
        {
            std::lock_guard<std::mutex> g(lock);
            strlcpy(note, W.stop_note, sizeof(note));
        }
        web_settings_stop(note);
    };
    a.name = "web-stop";
    esp_timer_create(&a, &W.stop_soon);
    a.callback = [](void *) {
        web_settings_stop("");
        if (W.restart) {
            W.restart();
        }
    };
    a.name = "web-restart";
    esp_timer_create(&a, &W.restart_soon);
}
}  // namespace

bool web_settings_start()
{
    std::lock_guard<std::mutex> g(lock);
    if (W.server) {
        return true;
    }
    make_timers();
    if (WiFi.status() != WL_CONNECTED) {
        strlcpy(W.note, "Connect to Wi-Fi first", sizeof(W.note));
        return false;
    }
    snprintf(W.url, sizeof(W.url), "http://%s", WiFi.localIP().toString().c_str());
    snprintf(W.code, sizeof(W.code), "%06lu", static_cast<unsigned long>(esp_random() % 1000000));
    mbedtls_platform_zeroize(W.sessions, sizeof(W.sessions));
    W.wrong = 0, W.note[0] = 0;
    W.last = esp_timer_get_time();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;  // JSON both ways; its saves write flash, so an internal stack
    cfg.task_priority = 3;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 6;
    if (httpd_start(&W.server, &cfg) != ESP_OK) {
        W.server = nullptr;
        strlcpy(W.note, "Could not start the web server", sizeof(W.note));
        return false;
    }
    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, page, nullptr},
        {"/api/login", HTTP_POST, login, nullptr},
        {"/api/status", HTTP_GET, status, nullptr},
        {"/api/save", HTTP_POST, save, nullptr},
        {"/api/stop", HTTP_POST, stop, nullptr},
    };
    for (const httpd_uri_t &u : routes) {
        httpd_register_uri_handler(W.server, &u);
    }
    esp_timer_start_periodic(W.idle, 15 * 1000000);
    ESP_LOGI(TAG, "on at %s", W.url);
    return true;
}

void web_settings_stop(const char *note)
{
    httpd_handle_t server;
    {
        std::lock_guard<std::mutex> g(lock);
        server = W.server;
        W.server = nullptr;
        mbedtls_platform_zeroize(W.code, sizeof(W.code));
        mbedtls_platform_zeroize(W.sessions, sizeof(W.sessions));
        if (note) {
            strlcpy(W.note, note, sizeof(W.note));
        }
        if (W.idle) {
            esp_timer_stop(W.idle);
        }
    }
    if (server) {
        httpd_stop(server);  // outside the lock: a request being served takes it
        ESP_LOGI(TAG, "off%s%s", note && *note ? ": " : "", note ? note : "");
    }
}

void web_settings_info(WebSettingsInfo *out)
{
    std::lock_guard<std::mutex> g(lock);
    out->running = W.server != nullptr;
    strlcpy(out->url, out->running ? W.url : "", sizeof(out->url));
    strlcpy(out->code, out->running ? W.code : "", sizeof(out->code));
    strlcpy(out->note, W.note, sizeof(out->note));
}

void web_settings_set_restart(void (*restart)())
{
    W.restart = restart;
}
