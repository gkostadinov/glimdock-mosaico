// SPDX-License-Identifier: Apache-2.0
#include "swifty_config.h"

#include <string.h>

#include <mutex>
#include <vector>

#include "ArduinoJson.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "nvs.h"

namespace swifty {
namespace {
const char *TAG = "swifty-cfg";
// Its own namespace: Home keeps its hub pairing in "swifty".
constexpr const char *NS = "swifty_agent";
constexpr uint16_t RPC_SERVICE = 0x4744, RPC_CONFIG = 40;
constexpr size_t MAX_FRAME = 16 * 1024;
const char *const KEYS[] = {"llm_url", "llm_key", "llm_model", "asr_url", "asr_key", "asr_model", "asr_ca", "search_key"};
constexpr int NKEYS = 8, CA_KEY = 6;

std::mutex lock;
Config current;
unsigned revision = 0;
struct Section {
    std::string name;
    SectionApply apply;
    SectionStatus status;
};
std::vector<Section> sections;
// The frame being received: begin (total size), chunks, commit.
std::string frame;
size_t expected = 0;

std::string *field(Config &c, int i)
{
    std::string *fields[] = {&c.llm_url, &c.llm_key, &c.llm_model, &c.asr_url, &c.asr_key, &c.asr_model, &c.asr_ca, &c.search_key};
    return fields[i];
}

void wipe(std::string &s)
{
    volatile char *p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

void load()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    Config c;
    for (int i = 0; i < NKEYS; ++i) {
        size_t n = 0;
        if (nvs_get_str(h, KEYS[i], nullptr, &n) == ESP_OK && n > 1) {
            std::string v(n, '\0');
            if (nvs_get_str(h, KEYS[i], v.data(), &n) == ESP_OK) {
                v.resize(n - 1);
                *field(c, i) = v;
            }
        }
    }
    nvs_close(h);
    std::lock_guard<std::mutex> g(lock);
    current = c;
}

// Swifty's own keys; a key with an empty string is cleared, a missing key kept.
const char *apply_swifty(JsonVariant doc)
{
    Config next;
    {
        std::lock_guard<std::mutex> g(lock);
        next = current;
    }
    for (int i = 0; i < NKEYS; ++i) {
        JsonVariant v = doc[KEYS[i]];
        if (v.isNull()) continue;
        if (!v.is<const char *>()) return "values must be strings";
        const char *text = v.as<const char *>();
        if (strlen(text) > (i == CA_KEY ? 8000u : 400u)) return "a value is too long";
        *field(next, i) = text;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return "storage unavailable";
    esp_err_t err = ESP_OK;
    for (int i = 0; i < NKEYS && err == ESP_OK; ++i) {
        const std::string &v = *field(next, i);
        err = v.empty() ? nvs_erase_key(h, KEYS[i]) : nvs_set_str(h, KEYS[i], v.c_str());
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return "storage write failed";
    std::lock_guard<std::mutex> g(lock);
    current = next;
    ++revision;
    return nullptr;
}

bool is_swifty_key(const char *k)
{
    for (const char *key : KEYS)
        if (!strcmp(key, k)) return true;
    return false;
}

// The committed frame: {"swifty": {...}, "glimdock": {...}, "home": {...}}
// (or Swifty's keys at the top level). Sets reply to "ok", "ok restart" or
// the first error.
void commit(std::string &reply)
{
    JsonDocument doc;
    if (deserializeJson(doc, frame) || !doc.is<JsonObject>()) {
        reply = "invalid JSON";
        return;
    }
    bool flat = false;
    for (JsonPair kv : doc.as<JsonObject>()) {
        const char *k = kv.key().c_str();
        if (is_swifty_key(k)) {
            flat = true;
            continue;
        }
        bool known = !strcmp(k, "swifty");
        for (const Section &sec : sections) known |= sec.name == k;
        if (!known) {
            reply = std::string("unknown section \"") + k + "\"";
            return;
        }
    }
    JsonVariant sw = flat ? doc.as<JsonVariant>() : doc["swifty"].as<JsonVariant>();
    if (!sw.isNull()) {
        if (!sw.is<JsonObject>()) {
            reply = "swifty: expected an object";
            return;
        }
        if (const char *error = apply_swifty(sw)) {
            reply = std::string("swifty: ") + error;
            return;
        }
        ESP_LOGI(TAG, "Swifty configuration saved");
    }
    bool restart = false;
    for (const Section &sec : sections) {
        JsonVariant part = doc[sec.name];
        if (part.isNull()) continue;
        std::string json;
        serializeJson(part, json);
        const char *error = sec.apply(json.c_str(), json.size());
        for (char &c : json) c = 0;
        if (error && !strcmp(error, "restart")) restart = true;
        else if (error) {
            reply = sec.name + ": " + error;
            return;
        }
        ESP_LOGI(TAG, "%s configuration saved", sec.name.c_str());
    }
    reply = restart ? "ok restart" : "ok";
}

// Payload: op byte. 0 status; 1 begin (u32 size); 2 chunk; 3 commit; 4 abort.
// Replies are short words, never configuration values except the URLs.
esp_err_t rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    if (request->payload_size < 1) return ESP_ERR_INVALID_ARG;
    const uint8_t op = request->payload[0];
    const uint8_t *data = request->payload + 1;
    const size_t n = request->payload_size - 1;
    std::string reply = "ok";
    if (op == 0) {
        const Config c = config();
        JsonDocument doc;
        JsonObject sw = doc["swifty"].to<JsonObject>();
        sw["llm_url"] = c.llm_url;
        sw["llm_model"] = c.llm_model;
        sw["llm_key"] = !c.llm_key.empty();
        sw["asr_url"] = c.asr_url;
        sw["asr_model"] = c.asr_model;
        sw["asr_key"] = !c.asr_key.empty();
        sw["asr_ca"] = !c.asr_ca.empty();
        sw["search_key"] = !c.search_key.empty();
        for (const Section &sec : sections) {
            std::string text(1024, '\0');  // on the heap: the ESP-Iris task has a small stack
            sec.status(text.data(), text.size());
            JsonDocument part;
            if (deserializeJson(part, text.c_str()) == DeserializationError::Ok) doc[sec.name] = part;
        }
        reply.clear();
        serializeJson(doc, reply);
    } else if (op == 1 && n >= 4) {
        uint32_t total;
        memcpy(&total, data, 4);
        wipe(frame);
        if (total == 0 || total > MAX_FRAME) reply = "too large";
        else expected = total, frame.reserve(total);
    } else if (op == 2) {
        if (frame.size() + n > expected) reply = "unexpected data";
        else frame.append(reinterpret_cast<const char *>(data), n);
    } else if (op == 3) {
        if (frame.size() == expected) commit(reply);
        else reply = "incomplete";
        wipe(frame);
        expected = 0;
    } else if (op == 4) {
        wipe(frame);
        expected = 0;
    } else {
        reply = "unknown operation";
    }
    *size = std::min(reply.size(), capacity);
    memcpy(out, reply.data(), *size);
    return ESP_OK;
}
}  // namespace

Config config()
{
    std::lock_guard<std::mutex> g(lock);
    return current;
}

unsigned config_revision()
{
    std::lock_guard<std::mutex> g(lock);
    return revision;
}

void config_section(const char *name, SectionApply apply, SectionStatus status)
{
    std::lock_guard<std::mutex> g(lock);
    sections.push_back({name, apply, status});
}

void config_start()
{
    static bool started = false;
    if (started) return;
    started = true;
    load();
    esp_iris_rpc_register(RPC_SERVICE, RPC_CONFIG, rpc, nullptr);
}

}  // namespace swifty
