// SPDX-License-Identifier: Apache-2.0
#include "swifty_asr.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "ArduinoJson.h"
#include "board_mic.h"
#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "esp_transport.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "swifty_config.h"

namespace swifty {
namespace {
const char *TAG = "swifty-asr";
constexpr int RATE = 16000, FRAME_MS = 20, CHUNK_MS = 80;
constexpr size_t BYTES_PER_MS = RATE / 1000 * 2;
constexpr size_t FRAME_BYTES = BYTES_PER_MS * FRAME_MS, CHUNK_BYTES = BYTES_PER_MS * CHUNK_MS;
// The audio in a ring that wraps, so a dictation is not limited by its size:
// it keeps what was not sent yet and, for a reconnect, what came after the
// last words the server gave for good. Recording ends at MAX_SPEECH_MS (a
// stuck key).
constexpr size_t RING_BYTES = RATE * 2 * 30;
constexpr size_t MAX_SPEECH_BYTES = BYTES_PER_MS * 5 * 60 * 1000;
static_assert(RING_BYTES % FRAME_BYTES == 0, "frames never wrap");
// A connection that drops during a dictation (the server ends sessions after
// about a minute; a phone hotspot loses a few packets) is made again and the
// audio since the last final words sent again, this many times.
constexpr int MAX_RECONNECTS = 4;
// The speech host's TLS handshake costs ~1.3 s of CPU at priority 2 (software
// P-384), more on a slow path: 6 s ran out over a phone hotspot and the VPN.
constexpr int NETWORK_TIMEOUT_MS = 12000;
constexpr int CONNECT_WAIT_MS = 20000;  // DNS, TCP, TLS and the upgrade; the error event comes first
constexpr int START_WAIT_MS = 10000;    // run-task to task-started
constexpr int SEND_TIMEOUT_MS = 3000;   // one audio chunk: the socket's lock, then room to write
constexpr int64_t STALL_US = 15 * 1000000LL;  // nothing sent and no words for this long: it is stuck
// Task stacks in PSRAM: internal memory runs out while Swifty listens (the
// figure, Wi-Fi, LVGL, the microphone's DMA buffers), and every TLS or I2S
// allocation then failed. None of these tasks touch flash.
constexpr UBaseType_t PSRAM_STACK = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
// FINISHED is the server's task-finished; DONE is the session's end (the
// final words are in), GAVE_UP that it failed for good.
constexpr EventBits_t CONNECTED = BIT0, STARTED = BIT1, FINISHED = BIT2, FAILED = BIT3, MIC_DONE = BIT4, GAVE_UP = BIT5,
                      DONE = BIT6, WORKER_DONE = BIT7;

struct State {
    std::mutex lock;  // text, ring and offsets
    AsrEvents events;
    esp_codec_dev_handle_t mic = nullptr;
    esp_websocket_client_handle_t ws = nullptr;
    EventGroupHandle_t bits = nullptr;
    uint8_t *ring = nullptr;
    // Offsets in the audio since the press (bytes; ring position = offset %
    // RING_BYTES): captured, handed to the socket, the oldest still in the
    // ring, where the current task's audio began and where the final words end.
    size_t written = 0, sent = 0, keep_from = 0, task_from = 0, commit_at = 0;
    uint32_t dropped = 0;  // frames not recorded: the ring was full of unsent audio
    // active: from asr_start until asr_finish or asr_cancel returns.
    // worker: the session's connect, then streaming, task is running (it
    // owns the socket until it ends).
    std::atomic<bool> recording{false}, active{false}, cancelled{false}, worker{false};
    std::mutex socket_lock;  // ws: opened by the keeper or the worker, closed by them
    std::string committed, partial, rx, task_id, error;
    long long last_sentence = -1;
    int task_results = 0, reconnects = 0, fruitless = 0;  // fruitless: tasks in a row that failed without progress
    uint32_t noise = 0, envelope = 0, frames = 0;
    std::string ca;  // kept alive for the client
    // A connection made ahead of a press while Swifty is open: the handshake
    // takes seconds, so the press finds it ready (each recognition still gets
    // its own connection; the next is made once it is done).
    std::atomic<bool> idle_dead{false};    // the waiting connection was dropped
    std::atomic<int64_t> warm_until{0};    // keep one ready until then (esp_timer us)
    TaskHandle_t keeper = nullptr;
    // Timing for the log: when the socket was opened and connected, when the
    // press came, and the last sign of life (audio sent, words received).
    std::atomic<int64_t> opened_at{0}, connected_at{0}, pressed_at{0}, progress_at{0};
    // After a connect timed out: whether a plain TCP connection to the host
    // opened (1) or not (2), which tells a stalled TLS handshake from a host
    // that cannot be reached (0: not tried).
    std::atomic<int> reach{0};
} S;

int64_t now_us()
{
    return esp_timer_get_time();
}

void alive()
{
    S.progress_at = now_us();
}

// ---- loudness: ESP-Claw's envelope (noise floor tracking, compressed level) -------------
float loudness(const int16_t *pcm, size_t count)
{
    uint32_t sum = 0, n = 0;
    for (size_t i = 0; i < count; i += 4, ++n) {
        sum += uint32_t(abs(pcm[i]));
    }
    if (!n) {
        return 0;
    }
    const uint32_t mean = sum / n;
    if (S.frames++ == 0) {
        S.noise = mean < 512 ? mean : 160;
    } else if (mean > S.noise && mean - S.noise < 800) {
        S.noise += (mean - S.noise + 63) >> 6;
    } else if (mean < S.noise) {
        S.noise -= (S.noise - mean + 31) >> 5;
    }
    uint32_t signal = mean > S.noise ? mean - S.noise : 0, target;
    if (signal <= 24) {
        target = 0;
    } else if ((signal -= 24) < 512) {
        target = signal >> 2;
    } else if (signal < 2048) {
        target = 128 + ((signal - 512) >> 4);
    } else {
        target = std::min<uint32_t>(255, 224 + ((signal - 2048) >> 7));
    }
    if (target > S.envelope) {
        S.envelope += (target - S.envelope + 1) >> 1;
    } else {
        S.envelope -= (S.envelope - target + 7) >> 3;
    }
    return S.envelope / 255.f;
}

// ---- the ring (under S.lock) --------------------------------------------------------------
void ring_read(size_t from, uint8_t *out, size_t n)
{
    const size_t at = from % RING_BYTES, first = std::min(n, RING_BYTES - at);
    memcpy(out, S.ring + at, first);
    memcpy(out + first, S.ring, n - first);
}

// A frame in, unless that would overwrite audio not sent yet. Sent audio the
// ring needs the room for goes first (a reconnect then starts later).
bool ring_put(const int16_t *frame)
{
    if (S.written >= MAX_SPEECH_BYTES) {
        return false;
    }
    if (S.written + FRAME_BYTES > S.keep_from + RING_BYTES) {
        const size_t oldest = S.written + FRAME_BYTES - RING_BYTES;
        if (oldest > S.sent) {
            return false;
        }
        S.keep_from = oldest;
    }
    memcpy(S.ring + S.written % RING_BYTES, frame, FRAME_BYTES);
    S.written += FRAME_BYTES;
    return true;
}

// ---- microphone -------------------------------------------------------------------------
void mic_task(void *)
{
    int16_t frame[FRAME_BYTES / 2];
    uint32_t frames = 0, failures = 0, peak = 0;
    uint64_t sum = 0;
    while (S.recording) {
        if (esp_codec_dev_read(S.mic, frame, sizeof(frame)) != ESP_CODEC_DEV_OK) {
            ++failures;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (size_t i = 0; i < FRAME_BYTES / 2; i += 4) {
            const uint32_t v = uint32_t(abs(frame[i]));
            sum += v;
            peak = std::max(peak, v);
        }
        if (++frames % 50 == 0) {  // once a second: is the microphone hearing anything?
            ESP_LOGI(TAG, "mic: mean %u peak %u (read failures %u)", unsigned(sum / (50 * FRAME_BYTES / 8)), unsigned(peak),
                     unsigned(failures));
            sum = 0, peak = 0;
        }
        const float level = loudness(frame, FRAME_BYTES / 2);
        {
            std::lock_guard<std::mutex> g(S.lock);
            if (!ring_put(frame) && S.dropped++ % 50 == 0) {
                ESP_LOGW(TAG, "%s: not recording", S.written >= MAX_SPEECH_BYTES ? "5 minutes reached" : "audio not sent for 30 s");
            }
        }
        if (S.events.level) {
            S.events.level(level);
        }
    }
    esp_codec_dev_close(S.mic);
    board_mic_release("swifty");
    xEventGroupSetBits(S.bits, MIC_DONE);
    vTaskDeleteWithCaps(nullptr);
}

// ---- socket -----------------------------------------------------------------------------
bool send_json(const JsonDocument &doc)
{
    std::string text;
    serializeJson(doc, text);
    return S.ws && esp_websocket_client_send_text(S.ws, text.c_str(), int(text.size()), pdMS_TO_TICKS(3000)) >= 0;
}

void run_task()
{
    JsonDocument doc;
    doc["header"]["action"] = "run-task";
    doc["header"]["task_id"] = S.task_id;
    doc["header"]["streaming"] = "duplex";
    JsonObject payload = doc["payload"].to<JsonObject>();
    payload["task_group"] = "audio";
    payload["task"] = "asr";
    payload["function"] = "recognition";
    payload["model"] = config().asr_model;
    payload["parameters"]["format"] = "pcm";
    payload["parameters"]["sample_rate"] = RATE;
    payload["input"].to<JsonObject>();
    send_json(doc);
}

bool finish_task()
{
    JsonDocument doc;
    doc["header"]["action"] = "finish-task";
    doc["header"]["task_id"] = S.task_id;
    doc["header"]["streaming"] = "duplex";
    doc["payload"]["input"].to<JsonObject>();
    return send_json(doc);
}

void handle_text(const std::string &text)
{
    JsonDocument doc;
    if (deserializeJson(doc, text)) {
        return;
    }
    const char *event = doc["header"]["event"] | "";
    if (strcmp(event, "result-generated")) {
        ESP_LOGI(TAG, "%s", event);
    }
    if (!strcmp(event, "task-started")) {
        ESP_LOGI(TAG, "recognising %lld ms after the press", (long long)((now_us() - S.pressed_at) / 1000));
        alive();
        xEventGroupSetBits(S.bits, STARTED);
    } else if (!strcmp(event, "task-finished")) {
        xEventGroupSetBits(S.bits, FINISHED);
    } else if (!strcmp(event, "task-failed")) {
        const char *code = doc["header"]["error_code"] | "";
        const char *message = doc["header"]["error_message"] | "";
        ESP_LOGW(TAG, "task failed: %s %s", code, message);
        std::lock_guard<std::mutex> g(S.lock);
        S.error = *message ? std::string("The speech service failed: ") + message : "Speech recognition failed.";
        xEventGroupSetBits(S.bits, FAILED);
    } else if (!strcmp(event, "result-generated")) {
        JsonVariant sentence = doc["payload"]["output"]["sentence"];
        if (sentence.isNull() || (sentence["heartbeat"] | false)) {
            return;
        }
        alive();
        const char *words = sentence["text"] | "";
        const bool end = sentence["sentence_end"] | false;
        const long long id = sentence["sentence_id"].is<long long>() ? sentence["sentence_id"].as<long long>() : -1;
        std::string snapshot;
        {
            std::lock_guard<std::mutex> g(S.lock);
            ++S.task_results;
            const long long sid = id < 0 ? S.last_sentence + 1 : id;
            if (end && sid > S.last_sentence) {
                S.committed += words;
                S.last_sentence = sid;
                S.partial.clear();
                // Where these words end in the audio, for a reconnect: the
                // sentence's end_time (ms of this task's audio) when the server
                // gives it, else what was sent by now (a sentence ends after a
                // pause is heard, so what lies between is mostly that pause).
                size_t at = S.sent;
                const long long ms = sentence["end_time"].is<long long>() ? sentence["end_time"].as<long long>() : -1;
                if (ms > 0) {
                    at = std::min(at, S.task_from + size_t(ms) * BYTES_PER_MS / FRAME_BYTES * FRAME_BYTES);
                }
                S.commit_at = std::max(S.commit_at, at);
            } else if (!end && sid > S.last_sentence) {
                S.partial = words;
            }
            snapshot = S.committed + S.partial;
        }
        if (!snapshot.empty() && S.events.partial) {
            S.events.partial(snapshot);
        }
    }
}

// A plain TCP connection to the host, after a connect timed out: it opens
// when only the TLS handshake stalled (big packets lost, as through a VPN
// whose MTU is too large for the path), not when the host cannot be reached.
// Runs in the connect helper (swifty-tls), which holds no lock.
void probe_tcp(const char *host, int port)
{
    struct addrinfo hints = {}, *found = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    char service[8];
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &found) != 0 || !found) {
        return;
    }
    int result = 2;
    const int fd = socket(found->ai_family, SOCK_STREAM, 0);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        if (connect(fd, found->ai_addr, found->ai_addrlen) == 0) {
            result = 1;
        } else if (errno == EINPROGRESS) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(fd, &w);
            struct timeval tv = {3, 0};
            int err = 0;
            socklen_t len = sizeof(err);
            if (select(fd + 1, nullptr, &w, nullptr, &tv) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && !err) {
                result = 1;
            }
        }
        close(fd);
    }
    freeaddrinfo(found);
    S.reach = result;
    ESP_LOGW(TAG, "connect timed out; a plain TCP connection to the speech host %s", result == 1 ? "opens (the TLS handshake stalled)" : "does not open either");
}

// What went wrong, in words (the codes go to the log). Only from the
// DISCONNECTED event: the client sends its ERROR event before it records the
// error's type and TLS codes (esp_websocket_client 1.8), so there every
// connect failure looked unknown ("unreachable") and the fields held
// leftovers.
std::string failure(const esp_websocket_event_data_t *e)
{
    if (!e) {
        return "The speech service is unreachable.";
    }
    const esp_websocket_error_codes_t &h = e->error_handle;
    if (h.error_type == WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT) {
        return "The speech service stopped answering.";
    }
    if (h.error_type != WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
        return "The speech service hung up.";
    }
    ESP_LOGW(TAG, "error: HTTP %d, tls 0x%x/%d, cert 0x%x, errno %d", h.esp_ws_handshake_status_code,
             unsigned(h.esp_tls_last_esp_err), h.esp_tls_stack_err, unsigned(h.esp_tls_cert_verify_flags),
             h.esp_transport_sock_errno);
    char text[120];
    const int status = h.esp_ws_handshake_status_code;
    if (status == 401 || status == 403) {
        return "The speech service refused the key.";
    }
    if (status >= 500) {
        snprintf(text, sizeof(text), "The speech service is up but failing (HTTP %d).", status);
        return text;
    }
    if (status > 0 && status != 101) {
        snprintf(text, sizeof(text), "The speech service answered HTTP %d.", status);
        return text;
    }
    switch (h.esp_tls_last_esp_err) {
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
        return "Can't find the speech server's address (DNS): its name doesn't resolve on this network.";
    case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
        return S.reach == 1 ? "The speech server answers, but the secure handshake stalled (on a VPN, try a lower MTU)."
               : "Couldn't reach the speech server (timed out connecting).";
    case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
        return "Can't connect to the speech server from this network.";
    case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED:
        if (h.esp_tls_cert_verify_flags) {
            snprintf(text, sizeof(text), "The speech server's certificate was rejected (0x%x).", unsigned(h.esp_tls_cert_verify_flags));
            return text;
        }
        return "The secure connection to the speech server failed.";
    default:
        break;
    }
    return "The connection to the speech server dropped.";
}

void on_event(void *, esp_event_base_t, int32_t id, void *data)
{
    auto *e = static_cast<esp_websocket_event_data_t *>(data);
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        S.connected_at = now_us();
        ESP_LOGI(TAG, "connected in %lld ms%s", (long long)((S.connected_at - S.opened_at) / 1000),
                 S.active ? "" : " (ready for the next press)");
        S.idle_dead = false;
        alive();
        xEventGroupSetBits(S.bits, CONNECTED);
        break;
    case WEBSOCKET_EVENT_ERROR:
        break;  // logged by the client; DISCONNECTED follows with the cause
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED: {
        const int64_t age = S.connected_at ? (now_us() - S.connected_at) / 1000 : -1;
        std::string why = failure(e);
        if (id == WEBSOCKET_EVENT_CLOSED) {
            why = e && e->close_status_code ? "The speech service closed the connection (code " + std::to_string(e->close_status_code) + ")."
                  : "The speech service closed the connection.";
        }
        ESP_LOGI(TAG, "socket %s%s (connected %lld ms before): %s", id == WEBSOCKET_EVENT_CLOSED ? "closed" : "disconnected",
                 S.active ? "" : " while waiting", (long long)age, why.c_str());
        if (!S.active || !S.worker) {  // the waiting connection: the keeper makes another
            S.idle_dead = true;
            xEventGroupClearBits(S.bits, CONNECTED);
            break;
        }
        if (S.cancelled) {
            break;
        }
        {
            std::lock_guard<std::mutex> g(S.lock);
            if (S.error.empty() && !(xEventGroupGetBits(S.bits) & FINISHED)) {
                S.error = why;
            }
        }
        xEventGroupSetBits(S.bits, FAILED);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (!e || e->op_code != 0x1 || e->payload_len <= 0 || e->payload_len > 16384) {
            break;
        }
        if (e->payload_offset == 0) {
            S.rx.clear();
        }
        S.rx.append(e->data_ptr, size_t(e->data_len));
        if (int(S.rx.size()) >= e->payload_len) {
            handle_text(S.rx);
            S.rx.clear();
        }
        break;
    default:
        break;
    }
}

void close_socket()
{
    std::lock_guard<std::mutex> g(S.socket_lock);
    if (!S.ws) {
        return;
    }
    esp_websocket_client_close(S.ws, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(S.ws);
    S.ws = nullptr;
    S.connected_at = 0;
}

// The keeper's: only a waiting connection no session uses (a press may have
// taken it over meanwhile).
void close_if_idle(bool only_dead)
{
    std::lock_guard<std::mutex> g(S.socket_lock);
    if (!S.ws || S.active || S.worker || (only_dead && !S.idle_dead)) {
        return;
    }
    esp_websocket_client_close(S.ws, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(S.ws);
    S.ws = nullptr;
    S.connected_at = 0;
}

bool open_socket(const Config &c)
{
    std::lock_guard<std::mutex> g(S.socket_lock);
    S.opened_at = now_us();
    S.connected_at = 0;
    S.reach = 0;
    esp_websocket_client_config_t ws = {};
    ws.uri = c.asr_url.c_str();
    ws.buffer_size = 8192;
    ws.network_timeout_ms = NETWORK_TIMEOUT_MS;
    ws.disable_auto_reconnect = true;
    // The TLS handshake takes seconds (software P-384): below LVGL (3), or the
    // screen froze for its length when the mic started.
    ws.task_prio = 2;
    ws.task_stack = 6144;
    ws.task_name = "swifty-ws";  // its connects run in a helper (__wrap_esp_transport_connect)
    ws.user_agent = "swifty-mosaico";
    if (!S.ca.empty()) {
        ws.cert_pem = S.ca.c_str();
    } else {
        ws.crt_bundle_attach = esp_crt_bundle_attach;
    }
    S.ws = esp_websocket_client_init(&ws);
    if (!S.ws) {
        return false;
    }
    const std::string auth = "Bearer " + c.asr_key;
    esp_websocket_client_append_header(S.ws, "Authorization", auth.c_str());
    esp_websocket_register_events(S.ws, WEBSOCKET_EVENT_ANY, on_event, nullptr);
    if (esp_websocket_client_start(S.ws) == ESP_OK) {
        return true;
    }
    esp_websocket_client_destroy(S.ws);
    S.ws = nullptr;
    return false;
}

void set_error(const char *text)
{
    std::lock_guard<std::mutex> g(S.lock);
    if (S.error.empty()) {
        S.error = text;
    }
}

// Waits for the open socket to connect. false (S.error set) when it did not.
bool wait_connected()
{
    const EventBits_t got = xEventGroupWaitBits(S.bits, CONNECTED | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_WAIT_MS));
    if ((got & CONNECTED) && !(got & FAILED)) {
        return true;
    }
    if (!(got & FAILED)) {
        set_error("The speech server didn't answer in time.");
    }
    return false;
}

// A recognition task on the connected socket: run-task, then task-started.
bool start_task()
{
    char id[40];
    snprintf(id, sizeof(id), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    S.task_id = id;
    run_task();
    const EventBits_t got = xEventGroupWaitBits(S.bits, STARTED | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(START_WAIT_MS));
    if ((got & STARTED) && !(got & FAILED)) {
        return true;
    }
    if (!(got & FAILED)) {
        set_error("The speech service connected but didn't start listening (no answer in 10 s).");
    }
    return false;
}

// The worker's end: the socket closed (nobody else touches it while the
// worker runs), then the session may end.
void worker_exit(EventBits_t result)
{
    xEventGroupSetBits(S.bits, result);
    close_socket();
    S.worker = false;
    xEventGroupSetBits(S.bits, WORKER_DONE);
    vTaskDeleteWithCaps(nullptr);
}

// Sends the audio until it is all sent and recording has ended. false when
// the connection failed or the server ended the task first.
bool stream_audio(uint8_t *chunk)
{
    int64_t last_sent = now_us();
    for (;;) {
        size_t n = 0, from = 0;
        bool done;
        {
            std::lock_guard<std::mutex> g(S.lock);
            done = !S.recording && S.sent >= S.written;
            if (S.written - S.sent >= CHUNK_BYTES || (!S.recording && S.written > S.sent)) {
                n = std::min(CHUNK_BYTES, S.written - S.sent);
                from = S.sent;
                ring_read(from, chunk, n);
            }
        }
        if (S.cancelled || (xEventGroupGetBits(S.bits) & (FAILED | FINISHED))) {
            return false;
        }
        if (done) {
            return true;
        }
        if (!n) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Counted as sent only once the socket took it: a send that timed out
        // (the socket's lock held, no room to write) is tried again.
        if (esp_websocket_client_send_bin(S.ws, reinterpret_cast<const char *>(chunk), int(n), pdMS_TO_TICKS(SEND_TIMEOUT_MS)) ==
                int(n)) {
            std::lock_guard<std::mutex> g(S.lock);
            if (S.sent == from) {
                S.sent += n;
            }
            last_sent = now_us();
            alive();
        } else if (now_us() - last_sent > STALL_US) {
            set_error("Sending the audio stalled.");
            return false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// The connection dropped (or the server ended the task) before the final
// words: a new connection and task, and the audio since the last final words
// once more (the words not final yet are heard again).
bool reconnect(const Config &c, int64_t speech_ms)
{
    std::string why;
    {
        std::lock_guard<std::mutex> g(S.lock);
        why = S.error.empty() ? "the server ended the task" : S.error;
    }
    close_socket();
    size_t from;
    {
        std::lock_guard<std::mutex> g(S.lock);
        from = std::min(std::max(S.commit_at, S.keep_from), S.sent);
        S.sent = S.task_from = S.commit_at = from;
        S.last_sentence = -1;
        S.task_results = 0;
        S.error.clear();
    }
    ESP_LOGW(TAG, "connection lost %lld ms into the speech (%s): connecting again, from %u ms of audio (%d of %d)",
             (long long)speech_ms, why.c_str(), unsigned(from / BYTES_PER_MS), S.reconnects, MAX_RECONNECTS);
    xEventGroupClearBits(S.bits, CONNECTED | STARTED | FINISHED | FAILED);
    if (S.cancelled) {
        return false;
    }
    if (!open_socket(c)) {
        set_error("Couldn't open the speech connection again (low on memory?).");
        return false;
    }
    return wait_connected() && start_task();
}

// Streams the audio, ends the task when recording has ended and waits for the
// final words; makes the connection again when it drops on the way.
void stream_task(void *)
{
    const Config c = config();
    uint8_t *chunk = static_cast<uint8_t *>(heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM));
    EventBits_t result = GAVE_UP;
    if (!chunk) {
        set_error("Couldn't start streaming (low on memory?).");
    }
    for (; chunk;) {
        const int64_t task_began = now_us();
        if (stream_audio(chunk) && !S.cancelled) {
            const bool asked = finish_task() || finish_task();
            // The final words: wait while the server is still working. All the
            // audio is there, so a server that goes quiet now is not asked
            // again: the words so far are the answer.
            bool quiet = false;
            while (asked && !S.cancelled) {
                const EventBits_t b = xEventGroupWaitBits(S.bits, FINISHED | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(200));
                if (b & (FINISHED | FAILED)) {
                    break;
                }
                if (now_us() - S.progress_at > STALL_US) {
                    set_error("The speech service didn't finish (no answer in 15 s).");
                    quiet = true;
                    break;
                }
            }
            // A server may hang up right after task-finished: that is done.
            if (xEventGroupGetBits(S.bits) & FINISHED) {
                result = DONE;
                break;
            }
            if (quiet) {
                break;
            }
        }
        if (S.cancelled) {
            break;
        }
        // Not again after two tasks in a row failed without progress (no words,
        // under 5 s of streaming): a server that fails every task at once is not
        // asked over and over. One such failure is a connection made ahead that
        // the server ended just after the press.
        const bool progressed = S.task_results > 0 || now_us() - task_began > 5 * 1000000LL;
        S.fruitless = progressed ? 0 : S.fruitless + 1;
        if (S.fruitless >= 2 || ++S.reconnects > MAX_RECONNECTS) {
            break;
        }
        if (!reconnect(c, (now_us() - S.pressed_at) / 1000)) {
            break;
        }
    }
    heap_caps_free(chunk);
    worker_exit(result);
}

// Connects (once more if the first try fails: the speech host sometimes
// answered "unavailable" or timed out while the audio kept recording), then
// starts the task and streams.
void connect_task(void *)
{
    const Config c = config();
    for (int attempt = 0; attempt < 2 && !S.cancelled; ++attempt) {
        if (attempt) {
            close_socket();
            ESP_LOGW(TAG, "speech service: trying again");
            {
                std::lock_guard<std::mutex> g(S.lock);
                S.error.clear();
            }
            xEventGroupClearBits(S.bits, CONNECTED | STARTED | FAILED);
            if (!open_socket(c)) {
                set_error("Couldn't open the speech connection (low on memory?).");
                continue;
            }
        } else if (!S.ws) {
            set_error("Couldn't open the speech connection (low on memory?).");
            continue;
        }
        if (!wait_connected() || !start_task()) {
            continue;
        }
        if (S.connected_at) {
            ESP_LOGI(TAG, "the connection was made %lld ms before the press",
                     (long long)((S.pressed_at - S.connected_at) / 1000));
        }
        if (xTaskCreatePinnedToCoreWithCaps(stream_task, "swifty-asr", 4096, nullptr, 5, nullptr, 0, PSRAM_STACK) == pdPASS) {
            vTaskDeleteWithCaps(nullptr);
        }
        set_error("Couldn't start streaming (low on memory?).");
        break;
    }
    set_error("The speech service is unreachable.");
    worker_exit(FAILED | GAVE_UP);
}
constexpr int64_t WARM_US = 180 * 1000000LL;  // keep a connection ready this long after Swifty was used

// While warm: one connected, idle socket for the next press (a new one after
// each recognition, or when the server drops the waiting one).
void keeper_task(void *)
{
    int64_t retry_at = 0;
    // Failed connections retry later each time (3, 6, 12, 24, then every 60 s):
    // each attempt is a ~1.3 s TLS handshake, and when memory is short every
    // one fails, which kept a core busy and slowed the whole board.
    int failures = 0;
    const auto backoff = [&failures] { return std::min<int64_t>(60, 3LL << std::min(failures++, 4)) * 1000000LL; };
    const auto idle = [] { return !S.active && !S.worker; };
    while (now_us() < S.warm_until.load()) {
        if (idle()) {
            bool missing, dropped;
            {
                std::lock_guard<std::mutex> g(S.socket_lock);
                missing = !S.ws;
                dropped = S.ws && S.idle_dead;
            }
            if (dropped) {
                close_if_idle(true);
            }
            const int64_t now = now_us();
            const Config c = config();
            if ((missing || dropped) && now >= retry_at && c.asr_ready() && idle()) {
                S.ca = c.asr_ca;
                S.idle_dead = false;
                xEventGroupClearBits(S.bits, CONNECTED);
                if (!open_socket(c)) {
                    retry_at = now + std::max<int64_t>(10 * 1000000LL, backoff());
                } else {
                    // Connected, failed, or a press took it over.
                    for (int i = 0; i < CONNECT_WAIT_MS / 100 && idle() && !S.idle_dead && !(xEventGroupGetBits(S.bits) & CONNECTED);
                            ++i) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    if (xEventGroupGetBits(S.bits) & CONNECTED) {
                        failures = 0;
                    } else if (idle()) {
                        close_if_idle(false);  // did not connect: again in a while
                        retry_at = now_us() + (S.idle_dead ? backoff() : std::max<int64_t>(15 * 1000000LL, backoff()));
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    close_if_idle(false);  // nobody is coming: let the server go
    S.keeper = nullptr;
    vTaskDeleteWithCaps(nullptr);
}
}  // namespace

bool asr_active()
{
    return S.active;
}

void asr_warm(bool on)
{
    if (!on) {
        S.warm_until = 0;  // the keeper closes the waiting connection and ends
        return;
    }
    if (!config().asr_ready()) {
        return;
    }
    if (!S.bits) {
        S.bits = xEventGroupCreate();
    }
    S.warm_until = now_us() + WARM_US;
    if (!S.keeper) {
        xTaskCreatePinnedToCoreWithCaps(keeper_task, "swifty-warm", 4096, nullptr, 2, &S.keeper, 0, PSRAM_STACK);
    }
}

bool asr_start(const AsrEvents &events)
{
    if (S.active || S.worker) {
        if (S.worker) {
            ESP_LOGW(TAG, "the last recognition is still closing");
        }
        return false;
    }
    S.pressed_at = now_us();
    const Config c = config();
    if (!c.asr_ready()) {
        return false;
    }
    if (!S.bits) {
        S.bits = xEventGroupCreate();
    }
    if (!S.ring) {
        S.ring = static_cast<uint8_t *>(heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM));
    }
    if (!S.mic) {
        S.mic = bsp_audio_codec_microphone_init();
        if (!S.mic) {
            ESP_LOGW(TAG, "microphone unavailable");
            return false;
        }
    }
    if (!S.ring) {
        return false;
    }
    // The computer may have it (webcam mode's USB microphone).
    if (!board_mic_claim("swifty")) {
        ESP_LOGW(TAG, "microphone in use (%s)", board_mic_owner() ? board_mic_owner() : "?");
        return false;
    }
    esp_codec_dev_sample_info_t format = {};
    format.bits_per_sample = 16;
    format.channel = 1;
    format.sample_rate = RATE;
    if (esp_codec_dev_open(S.mic, &format) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "microphone open failed");
        board_mic_release("swifty");
        return false;
    }
    esp_codec_dev_set_in_gain(S.mic, 30.0f);
    S.active = true;  // from here the keeper leaves the socket alone
    S.worker = true;
    if (S.warm_until.load() > now_us()) {
        S.warm_until = now_us() + WARM_US;
    }
    bool warm = false;
    {
        std::lock_guard<std::mutex> g(S.socket_lock);
        warm = S.ws && !S.idle_dead;  // connected, or still connecting
    }
    if (!warm) {
        close_socket();    // a dropped one
    }
    xEventGroupClearBits(S.bits, (warm ? 0 : CONNECTED) | STARTED | FINISHED | FAILED | MIC_DONE | GAVE_UP | DONE | WORKER_DONE);
    ESP_LOGI(TAG, "%s", warm ? "using the connection made ahead" : "connecting");
    {
        std::lock_guard<std::mutex> g(S.lock);
        S.events = events;
        S.written = S.sent = S.keep_from = S.task_from = S.commit_at = 0;
        S.dropped = 0;
        S.committed.clear();
        S.partial.clear();
        S.error.clear();
        S.last_sentence = -1;
        S.task_results = S.reconnects = S.fruitless = 0;
        S.frames = 0;
        S.envelope = 0;
    }
    alive();
    S.recording = true;
    S.cancelled = false;
    xTaskCreatePinnedToCoreWithCaps(mic_task, "swifty-mic", 4096, nullptr, 6, nullptr, 1, PSRAM_STACK);

    if (!warm) {
        S.ca = c.asr_ca;
        open_socket(c);  // a failure here is retried by the connect task
    }
    if (xTaskCreatePinnedToCoreWithCaps(connect_task, "swifty-asr0", 4096, nullptr, 5, nullptr, 0, PSRAM_STACK) != pdPASS) {
        S.recording = false;
        xEventGroupWaitBits(S.bits, MIC_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        close_socket();
        S.worker = false;
        S.active = false;
        return false;
    }
    return true;
}

std::string asr_finish(std::string *error)
{
    if (!S.active) {
        return "";
    }
    S.recording = false;
    const int64_t released = now_us();
    xEventGroupWaitBits(S.bits, MIC_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    // The worker ends every way by itself (each of its waits is bounded); this
    // limit is only a safety net.
    const EventBits_t b = xEventGroupWaitBits(S.bits, DONE | GAVE_UP, pdFALSE, pdFALSE, pdMS_TO_TICKS(120000));
    std::string text, err;
    size_t audio;
    uint32_t dropped;
    int reconnects;
    {
        std::lock_guard<std::mutex> g(S.lock);
        text = S.committed + S.partial;
        err = (b & DONE) ? "" : S.error.empty() ? "The speech service didn't answer." : S.error;
        audio = S.written;
        dropped = S.dropped;
        reconnects = S.reconnects;
        S.events = {};
    }
    S.cancelled = true;  // no retry after this
    xEventGroupSetBits(S.bits, FAILED);
    xEventGroupWaitBits(S.bits, WORKER_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(SEND_TIMEOUT_MS + 2000));
    S.active = false;
    ESP_LOGI(TAG, "finished %lld ms after the release: %u ms of audio, %u characters, %d reconnect(s)%s%s%s",
             (long long)((now_us() - released) / 1000), unsigned(audio / BYTES_PER_MS), unsigned(text.size()), reconnects,
             dropped ? ", frames lost" : "", err.empty() ? "" : ": ", err.c_str());
    if (text.empty() && err.empty()) {
        err = "I didn't catch anything.";
    }
    if (error) {
        // With words, the error says they may be cut short.
        *error = err;
    }
    return text;
}

void asr_cancel()
{
    if (!S.active) {
        return;
    }
    S.cancelled = true;
    S.recording = false;
    xEventGroupSetBits(S.bits, FAILED);
    {
        std::lock_guard<std::mutex> g(S.lock);
        S.events = {};
    }
    // The worker sees it at once (or after one send, at most a few seconds),
    // closes the socket and ends; a press meanwhile is refused.
    S.active = false;
}

}  // namespace swifty

// The websocket client connects (DNS, TCP, TLS, upgrade) while holding its own
// client lock. A moment's contention with Wi-Fi (23) or lwIP (18) on lwIP's
// lock raises its task to their priority, and FreeRTOS only drops an inherited
// priority once a task holds no mutex: the ~1.3 s handshake then ran at 18-23
// and froze the screen for 0.5-0.7 s at every reconnect (about once a minute
// while Swifty is open). The speech socket's connect runs in a helper task that
// holds no lock while its own task waits (linker --wrap, see CMakeLists.txt).
namespace {
struct Connect {
    esp_transport_handle_t t;
    const char *host;
    int port, timeout_ms, result, err;
    SemaphoreHandle_t done;
};
}  // namespace

extern "C" int __real_esp_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms);

extern "C" int __wrap_esp_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    if (strcmp(pcTaskGetName(nullptr), "swifty-ws") != 0) {
        return __real_esp_transport_connect(t, host, port, timeout_ms);
    }
    static SemaphoreHandle_t done = xSemaphoreCreateBinary();  // one speech socket at a time
    Connect c = {t, host, port, timeout_ms, -1, 0, done};
    // An internal stack, as the websocket task's: TLS may hand stack buffers to DMA.
    if (xTaskCreatePinnedToCore(
    [](void *arg) {
    Connect *c = static_cast<Connect *>(arg);
        c->result = __real_esp_transport_connect(c->t, c->host, c->port, c->timeout_ms);
        c->err = errno;
        const esp_tls_error_handle_t h = esp_transport_get_error_handle(c->t);
        if (c->result < 0 && h && h->last_error == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
            swifty::probe_tcp(c->host, c->port);
        }
        xSemaphoreGive(c->done);  // c is gone after this
        vTaskDelete(nullptr);
    },
    "swifty-tls", 6144, &c, 2, nullptr, tskNO_AFFINITY) != pdPASS)
    return __real_esp_transport_connect(t, host, port, timeout_ms);
    xSemaphoreTake(done, portMAX_DELAY);
    errno = c.err;
    return c.result;
}
