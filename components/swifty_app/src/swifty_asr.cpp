// SPDX-License-Identifier: Apache-2.0
#include "swifty_asr.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "ArduinoJson.h"
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
constexpr size_t FRAME_BYTES = RATE / 1000 * FRAME_MS * 2, CHUNK_BYTES = RATE / 1000 * CHUNK_MS * 2;
constexpr size_t RING_BYTES = RATE * 2 * 30;  // up to 30 s of speech
// Task stacks in PSRAM: internal memory runs out while Swifty listens (the
// figure, Wi-Fi, LVGL, the microphone's DMA buffers), and every TLS or I2S
// allocation then failed. None of these tasks touch flash.
constexpr UBaseType_t PSRAM_STACK = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr EventBits_t CONNECTED = BIT0, STARTED = BIT1, FINISHED = BIT2, FAILED = BIT3, MIC_DONE = BIT4, GAVE_UP = BIT5;

struct State {
    std::mutex lock;  // text and ring
    AsrEvents events;
    esp_codec_dev_handle_t mic = nullptr;
    esp_websocket_client_handle_t ws = nullptr;
    EventGroupHandle_t bits = nullptr;
    uint8_t *ring = nullptr;
    size_t written = 0, sent = 0;  // bytes captured and streamed
    std::atomic<bool> recording{false}, active{false}, cancelled{false}, streaming{false};
    std::mutex socket_lock;  // ws: opened by the connect task, closed by finish/cancel
    std::string committed, partial, rx, task_id, error;
    long long last_sentence = -1;
    uint32_t noise = 0, envelope = 0, frames = 0;
    std::string ca;  // kept alive for the client
    // A connection made ahead of a press while Swifty is open: the handshake
    // takes seconds, so the press finds it ready (each recognition still gets
    // its own connection; the next is made once it is done).
    std::atomic<bool> idle_dead{false};    // the waiting connection was dropped
    std::atomic<int64_t> warm_until{0};    // keep one ready until then (esp_timer us)
    TaskHandle_t keeper = nullptr;
    // Timing for the log: when the socket was opened and when the press came.
    std::atomic<int64_t> opened_at{0}, pressed_at{0};
} S;

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

// ---- microphone -------------------------------------------------------------------------
void mic_task(void *)
{
    int16_t frame[FRAME_BYTES / 2];
    uint32_t frames = 0, failures = 0, peak = 0;
    uint64_t sum = 0;
    while (S.recording) {
        if (esp_codec_dev_read(S.mic, frame, sizeof(frame)) != ESP_CODEC_DEV_OK) {
            ++failures;
            vTaskDelay(pdMS_TO_TICKS(10));  // at least a tick (100 Hz): 5 ms was 0, a spin
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
            if (S.written + FRAME_BYTES <= RING_BYTES) {
                memcpy(S.ring + S.written, frame, FRAME_BYTES);
                S.written += FRAME_BYTES;
            }
        }
        if (S.events.level) {
            S.events.level(level);
        }
    }
    esp_codec_dev_close(S.mic);
    xEventGroupSetBits(S.bits, MIC_DONE);
    vTaskDeleteWithCaps(nullptr);
}

// ---- socket -----------------------------------------------------------------------------
void send_json(const JsonDocument &doc)
{
    std::string text;
    serializeJson(doc, text);
    if (S.ws) {
        esp_websocket_client_send_text(S.ws, text.c_str(), int(text.size()), pdMS_TO_TICKS(3000));
    }
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

void finish_task()
{
    JsonDocument doc;
    doc["header"]["action"] = "finish-task";
    doc["header"]["task_id"] = S.task_id;
    doc["header"]["streaming"] = "duplex";
    doc["payload"]["input"].to<JsonObject>();
    send_json(doc);
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
        ESP_LOGI(TAG, "recognising %lld ms after the press", (long long)((esp_timer_get_time() - S.pressed_at) / 1000));
        xEventGroupSetBits(S.bits, STARTED);
    } else if (!strcmp(event, "task-finished")) {
        xEventGroupSetBits(S.bits, FINISHED);
    } else if (!strcmp(event, "task-failed")) {
        std::lock_guard<std::mutex> g(S.lock);
        S.error = doc["header"]["error_message"] | "Speech recognition failed.";
        xEventGroupSetBits(S.bits, FAILED);
    } else if (!strcmp(event, "result-generated")) {
        JsonVariant sentence = doc["payload"]["output"]["sentence"];
        if (sentence.isNull() || (sentence["heartbeat"] | false)) {
            return;
        }
        const char *words = sentence["text"] | "";
        const bool end = sentence["sentence_end"] | false;
        const long long id = sentence["sentence_id"].is<long long>() ? sentence["sentence_id"].as<long long>() : -1;
        std::string snapshot;
        {
            std::lock_guard<std::mutex> g(S.lock);
            const long long sid = id < 0 ? S.last_sentence + 1 : id;
            if (end && sid > S.last_sentence) {
                S.committed += words;
                S.last_sentence = sid;
                S.partial.clear();
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

// What went wrong, in words (the codes go to the log): the name not resolving,
// no connection, a timeout, the certificate, or the server's own answer.
std::string failure(const esp_websocket_event_data_t *e)
{
    if (!e) {
        return "The speech service is unreachable.";
    }
    const esp_websocket_error_codes_t &h = e->error_handle;
    ESP_LOGW(TAG, "error: type %d, HTTP %d, tls 0x%x/%d, cert 0x%x, errno %d", int(h.error_type),
             h.esp_ws_handshake_status_code, unsigned(h.esp_tls_last_esp_err), h.esp_tls_stack_err,
             unsigned(h.esp_tls_cert_verify_flags), h.esp_transport_sock_errno);
    // Each field only means something for its kind of error (the others can
    // hold anything: "certificate rejected" came from such leftovers).
    char text[96];
    switch (h.error_type) {
    case WEBSOCKET_ERROR_TYPE_HANDSHAKE: {
        const int status = h.esp_ws_handshake_status_code;
        if (status == 401 || status == 403) {
            return "The speech service refused the key.";
        }
        if (status >= 500) {
            snprintf(text, sizeof(text), "The speech service is up but failing (HTTP %d).", status);
        } else if (status > 0) {
            snprintf(text, sizeof(text), "The speech service answered HTTP %d.", status);
        } else {
            strlcpy(text, "The speech service's handshake failed.", sizeof(text));
        }
        return text;
    }
    case WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT:
        switch (h.esp_tls_last_esp_err) {
        case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME: return "The speech host's name doesn't resolve on this Wi-Fi.";
        case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT: return "The speech host didn't answer in time.";
        case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST: return "Can't connect to the speech host from this Wi-Fi.";
        case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED:
            if (h.esp_tls_cert_verify_flags) {
                snprintf(text, sizeof(text), "The speech host's certificate was rejected (0x%x).",
                         unsigned(h.esp_tls_cert_verify_flags));
                return text;
            }
            return "The secure connection to the speech host failed.";
        default: break;
        }
        return "The connection to the speech host dropped.";
    case WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT: return "The speech service stopped answering.";
    case WEBSOCKET_ERROR_TYPE_SERVER_CLOSE: return "The speech service closed the connection.";
    default: return "The speech service is unreachable.";
    }
}

void on_event(void *, esp_event_base_t, int32_t id, void *data)
{
    auto *e = static_cast<esp_websocket_event_data_t *>(data);
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected in %lld ms%s", (long long)((esp_timer_get_time() - S.opened_at) / 1000),
                 S.active ? "" : " (ready for the next press)");
        S.idle_dead = false;
        xEventGroupSetBits(S.bits, CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "socket %s", id == WEBSOCKET_EVENT_CLOSED ? "closed" : "disconnected");
        if (!S.active) {  // the waiting connection: the keeper makes another
            S.idle_dead = true;
            xEventGroupClearBits(S.bits, CONNECTED);
            break;
        }
        {
            std::lock_guard<std::mutex> g(S.lock);
            if (S.error.empty() && !(xEventGroupGetBits(S.bits) & FINISHED)) {
                S.error = "The speech service hung up.";
            }
        }
        xEventGroupSetBits(S.bits, FAILED);
        break;
    case WEBSOCKET_EVENT_ERROR: {
        const std::string why = failure(e);
        if (!S.active) {
            S.idle_dead = true;
            xEventGroupClearBits(S.bits, CONNECTED);
            break;
        }
        std::lock_guard<std::mutex> g(S.lock);
        if (S.error.empty()) {
            S.error = why;
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

// Streams captured audio while recording, then the rest, then finish-task.
void stream_task(void *)
{
    const Config c = config();
    EventBits_t got = xEventGroupWaitBits(S.bits, STARTED | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(6000));
    if (got & STARTED) {
        uint8_t *chunk = static_cast<uint8_t *>(heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM));
        for (;;) {
            size_t n = 0;
            bool done;
            {
                std::lock_guard<std::mutex> g(S.lock);
                done = !S.recording && S.sent >= S.written;
                if (S.written - S.sent >= CHUNK_BYTES || (!S.recording && S.written > S.sent)) {
                    n = std::min(CHUNK_BYTES, S.written - S.sent);
                    memcpy(chunk, S.ring + S.sent, n);
                    S.sent += n;
                }
            }
            if (done || (xEventGroupGetBits(S.bits) & FAILED)) {
                break;
            }
            if (n) {
                esp_websocket_client_send_bin(S.ws, reinterpret_cast<const char *>(chunk), int(n), pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        heap_caps_free(chunk);
        if (!(xEventGroupGetBits(S.bits) & FAILED)) {
            finish_task();
        }
    } else if (!(got & FAILED)) {
        std::lock_guard<std::mutex> g(S.lock);
        S.error = "The speech service did not start.";
        xEventGroupSetBits(S.bits, FAILED);
    }
    vTaskDeleteWithCaps(nullptr);
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
}

bool open_socket(const Config &c)
{
    std::lock_guard<std::mutex> g(S.socket_lock);
    S.opened_at = esp_timer_get_time();
    esp_websocket_client_config_t ws = {};
    ws.uri = c.asr_url.c_str();
    ws.buffer_size = 8192;
    ws.network_timeout_ms = 6000;
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
                continue;
            }
        } else if (!S.ws) {
            continue;
        }
        // The TLS handshake to the speech host has taken ~6 s: a slow connect
        // is waited for, only an actual failure is retried.
        const EventBits_t got = xEventGroupWaitBits(S.bits, CONNECTED | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
        if (!(got & CONNECTED) && !(got & FAILED)) {  // no answer at all: give up
            std::lock_guard<std::mutex> g(S.lock);
            if (S.error.empty()) {
                S.error = "The speech host didn't answer in time.";
            }
            break;
        }
        if (!(got & CONNECTED) || (got & FAILED)) {
            continue;
        }
        run_task();
        if (xEventGroupWaitBits(S.bits, STARTED | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000)) & STARTED) {
            S.streaming = true;
            xTaskCreatePinnedToCoreWithCaps(stream_task, "swifty-asr", 4096, nullptr, 5, nullptr, 0, PSRAM_STACK);
            vTaskDeleteWithCaps(nullptr);
        }
    }
    {
        std::lock_guard<std::mutex> g(S.lock);
        if (S.error.empty()) {
            S.error = "The speech service is unreachable.";
        }
    }
    xEventGroupSetBits(S.bits, FAILED | GAVE_UP);
    vTaskDeleteWithCaps(nullptr);
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
    while (esp_timer_get_time() < S.warm_until.load()) {
        if (!S.active) {
            bool missing, dropped;
            {
                std::lock_guard<std::mutex> g(S.socket_lock);
                missing = !S.ws;
                dropped = S.ws && S.idle_dead;
            }
            if (dropped && !S.active) {
                close_socket();
            }
            const int64_t now = esp_timer_get_time();
            const Config c = config();
            if ((missing || dropped) && now >= retry_at && c.asr_ready() && !S.active) {
                S.ca = c.asr_ca;
                S.idle_dead = false;
                xEventGroupClearBits(S.bits, CONNECTED);
                if (!open_socket(c)) {
                    retry_at = now + std::max<int64_t>(10 * 1000000LL, backoff());
                } else {
                    // Connected, failed, or a press took it over (up to 12 s).
                    for (int i = 0; i < 120 && !S.active && !S.idle_dead && !(xEventGroupGetBits(S.bits) & CONNECTED); ++i) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    if (xEventGroupGetBits(S.bits) & CONNECTED) {
                        failures = 0;
                    } else if (!S.active) {
                        close_socket();  // did not connect: again in a while
                        retry_at = esp_timer_get_time() + (S.idle_dead ? backoff() : std::max<int64_t>(15 * 1000000LL, backoff()));
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!S.active) {
        close_socket();    // nobody is coming: let the server go
    }
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
    S.warm_until = esp_timer_get_time() + WARM_US;
    if (!S.keeper) {
        xTaskCreatePinnedToCoreWithCaps(keeper_task, "swifty-warm", 4096, nullptr, 2, &S.keeper, 0, PSRAM_STACK);
    }
}

bool asr_start(const AsrEvents &events)
{
    if (S.active) {
        return false;
    }
    S.pressed_at = esp_timer_get_time();
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
    esp_codec_dev_sample_info_t format = {};
    format.bits_per_sample = 16;
    format.channel = 1;
    format.sample_rate = RATE;
    if (esp_codec_dev_open(S.mic, &format) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "microphone open failed");
        return false;
    }
    esp_codec_dev_set_in_gain(S.mic, 30.0f);
    S.active = true;  // from here the keeper leaves the socket alone
    if (S.warm_until.load() > esp_timer_get_time()) {
        S.warm_until = esp_timer_get_time() + WARM_US;
    }
    bool warm = false;
    {
        std::lock_guard<std::mutex> g(S.socket_lock);
        warm = S.ws && !S.idle_dead;  // connected, or still connecting
    }
    if (!warm) {
        close_socket();    // a dropped one
    }
    xEventGroupClearBits(S.bits, (warm ? 0 : CONNECTED) | STARTED | FINISHED | FAILED | MIC_DONE | GAVE_UP);
    ESP_LOGI(TAG, "%s", warm ? "using the connection made ahead" : "connecting");
    S.streaming = false;
    {
        std::lock_guard<std::mutex> g(S.lock);
        S.events = events;
        S.written = S.sent = 0;
        S.committed.clear();
        S.partial.clear();
        S.error.clear();
        S.last_sentence = -1;
        S.frames = 0;
        S.envelope = 0;
    }
    char id[40];
    snprintf(id, sizeof(id), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    S.task_id = id;
    S.recording = true;
    S.cancelled = false;
    xTaskCreatePinnedToCoreWithCaps(mic_task, "swifty-mic", 4096, nullptr, 6, nullptr, 1, PSRAM_STACK);

    if (!warm) {
        S.ca = c.asr_ca;
        open_socket(c);  // a failure here is retried by the connect task
    }
    xTaskCreatePinnedToCoreWithCaps(connect_task, "swifty-asr0", 4096, nullptr, 5, nullptr, 0, PSRAM_STACK);
    return true;
}

std::string asr_finish(std::string *error)
{
    if (!S.active) {
        return "";
    }
    S.recording = false;
    xEventGroupWaitBits(S.bits, MIC_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    // Done when the text is final, or it failed for good: a failure before
    // streaming may still be retried by the connect task.
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(20000);  // connect (up to 12 s) + the rest
    while (xTaskGetTickCount() < until) {
        const EventBits_t b = xEventGroupWaitBits(S.bits, FINISHED | GAVE_UP, pdFALSE, pdFALSE, pdMS_TO_TICKS(200));
        if ((b & (FINISHED | GAVE_UP)) || ((b & FAILED) && S.streaming)) {
            break;
        }
    }
    S.cancelled = true;  // no retry after this
    vTaskDelay(pdMS_TO_TICKS(50));
    close_socket();
    std::string text, err;
    {
        std::lock_guard<std::mutex> g(S.lock);
        text = S.committed + S.partial;
        err = S.error;
        S.events = {};
    }
    S.active = false;
    ESP_LOGI(TAG, "finished: %u bytes of audio sent, %u words%s%s", unsigned(S.sent), unsigned(text.size()),
             err.empty() ? "" : ", ", err.c_str());
    if (text.empty() && err.empty()) {
        err = "I didn't catch anything.";
    }
    if (error) {
        *error = text.empty() ? err : "";
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
    xEventGroupWaitBits(S.bits, MIC_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(120));  // let the streaming task see FAILED and stop sending
    close_socket();
    {
        std::lock_guard<std::mutex> g(S.lock);
        S.events = {};
    }
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
        xSemaphoreGive(c->done);  // c is gone after this
        vTaskDelete(nullptr);
    },
    "swifty-tls", 6144, &c, 2, nullptr, tskNO_AFFINITY) != pdPASS)
    return __real_esp_transport_connect(t, host, port, timeout_ms);
    xSemaphoreTake(done, portMAX_DELAY);
    errno = c.err;
    return c.result;
}
