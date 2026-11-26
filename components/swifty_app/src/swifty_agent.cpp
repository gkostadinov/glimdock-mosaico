// SPDX-License-Identifier: Apache-2.0
#include "swifty_agent.h"
#include "swifty_app.h"

#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "ArduinoJson.h"
#include "camera_app.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "home_app.h"
#include "mbedtls/base64.h"
#include "swifty_config.h"

namespace swifty {
namespace {
const char *TAG = "swifty-agent";
constexpr int MAX_ROUNDS = 5, KEEP_PHOTOS = 2, MAX_TURNS = 24;

struct Msg {
    std::string role, text, tool_calls, tool_call_id;
    std::vector<Jpeg> photos;
};

struct Job {
    std::string text;
    std::vector<Jpeg> photos;
    bool spoken;
    AgentEvents events;
};

std::mutex lock;  // history
std::vector<Msg> history;
std::atomic<bool> busy{false}, stop{false};
QueueHandle_t jobs = nullptr;
std::string model_cache;
unsigned model_revision = ~0u;

const char *SYSTEM =
    "You are Swifty, the friendly blue bird of Swifty Home, living on an ESP-Mosaico: a small round-cornered "
    "480x480 touch screen with a camera, a microphone and Wi-Fi. You help the person in front of you.\n"
    "Write for a small screen: short answers in Markdown (short paragraphs, **bold**, lists, `code`, small "
    "tables only when they help). Keep to about 80 words unless asked for more. No emoji.\n"
    "You can see through the board's camera with take_photo, show the person a photo with show_photo, read "
    "the home with home_status and the time with get_time. When the person asks what you see or shows you "
    "something, take a photo. For anything current or outside what you know (weather, news, prices, opening "
    "hours, facts you are unsure of) use web_search and answer from its results, naming the source briefly. "
    "Every app on this board also gives you tools: Glimdock (the homelab and Kordlet agents), Home (lights, "
    "music, TV, plugs), Camera and Gallery, Files (the NAS), Tools (torch, level) and the device itself "
    "(battery, brightness, opening apps). Use them to answer and to act when asked; say what you did. Ask "
    "before anything that cannot be undone. "
    "To show a picture from the web, use Markdown ![alt](https://...) with a direct JPEG or PNG link.\n"
    "Begin a reply with exactly one mood tag when it fits: [pleased] for good news or thanks, [sorry] when you "
    "cannot help or something failed, [confused] when the request is unclear. Otherwise no tag.";

const char *TOOLS = R"([
{"type":"function","function":{"name":"take_photo","description":"Take a photo with the board's camera now and look at it. The photo is attached in the next message.","parameters":{"type":"object","properties":{},"required":[]}}},
{"type":"function","function":{"name":"show_photo","description":"Show one of the camera's photos on the screen, in the chat.","parameters":{"type":"object","properties":{"index":{"type":"integer","description":"1 is the oldest photo; 0 or omitted is the newest."}},"required":[]}}},
{"type":"function","function":{"name":"home_status","description":"Read the home's lights, music, TV and plugs from Swifty Home.","parameters":{"type":"object","properties":{},"required":[]}}},
{"type":"function","function":{"name":"get_time","description":"The local date and time.","parameters":{"type":"object","properties":{},"required":[]}}},
{"type":"function","function":{"name":"web_search","description":"Search the web for current information (weather, news, facts). Returns a short answer and the top results with their links.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"What to search for, as a full question or keywords; include the place for weather."}},"required":["query"]}}}
])";

// Tools the other apps registered (swifty_app_add_tool).
struct Ext {
    std::string name, description, parameters;
    swifty_tool_fn fn;
};
std::mutex ext_lock;
std::vector<Ext> ext_tools;

std::string base_url(const Config &c)
{
    std::string base = c.llm_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.size() < 3 || base.compare(base.size() - 3, 3, "/v1") != 0) {
        base += "/v1";
    }
    return base;
}

std::string data_url(const Jpeg &jpeg)
{
    size_t n = 0;
    mbedtls_base64_encode(nullptr, 0, &n, jpeg->data(), jpeg->size());
    std::string out = "data:image/jpeg;base64,";
    const size_t head = out.size();
    out.resize(head + n);
    mbedtls_base64_encode(reinterpret_cast<unsigned char *>(&out[head]), n, &n, jpeg->data(), jpeg->size());
    out.resize(head + n);
    return out;
}

esp_http_client_handle_t client_for(const std::string &url, const Config &c, esp_http_client_method_t method, int timeout)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method;
    cfg.timeout_ms = timeout;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    cfg.disable_auto_redirect = true;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return nullptr;
    }
    if (!c.llm_key.empty()) {
        const std::string auth = "Bearer " + c.llm_key;
        esp_http_client_set_header(h, "Authorization", auth.c_str());
    }
    return h;
}

// The configured model, or the server's first one.
std::string model(const Config &c, std::string *error)
{
    if (!c.llm_model.empty()) {
        return c.llm_model;
    }
    if (model_revision == config_revision() && !model_cache.empty()) {
        return model_cache;
    }
    esp_http_client_handle_t h = client_for(base_url(c) + "/models", c, HTTP_METHOD_GET, 8000);
    std::string body;
    if (h && esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0 &&
            esp_http_client_get_status_code(h) == 200) {
        char buf[512];
        int n;
        while ((n = esp_http_client_read(h, buf, sizeof(buf))) > 0 && body.size() < 32768) {
            body.append(buf, size_t(n));
        }
    }
    if (h) {
        esp_http_client_cleanup(h);
    }
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
        model_cache = doc["data"][0]["id"] | "";
    }
    model_revision = config_revision();
    if (model_cache.empty() && error) {
        *error = "Swifty's model server did not answer.";
    }
    return model_cache;
}

std::string build_request(const std::string &model_name)
{
    JsonDocument doc;
    doc["model"] = model_name;
    doc["stream"] = true;
    doc["temperature"] = 0.6;
    JsonDocument tools;
    deserializeJson(tools, TOOLS);
    {
        std::lock_guard<std::mutex> g(ext_lock);
        for (const Ext &e : ext_tools) {
            JsonObject t = tools.add<JsonObject>();
            t["type"] = "function";
            t["function"]["name"] = e.name;
            t["function"]["description"] = e.description;
            JsonDocument params;
            if (deserializeJson(params, e.parameters) == DeserializationError::Ok) {
                t["function"]["parameters"] = params;
            } else {
                t["function"]["parameters"]["type"] = "object";
            }
        }
    }
    doc["tools"] = tools;
    JsonArray messages = doc["messages"].to<JsonArray>();
    char now[64];
    const time_t t = time(nullptr);
    struct tm local;
    localtime_r(&t, &local);
    strftime(now, sizeof(now), "%A %d %B %Y, %H:%M", &local);
    JsonObject sys = messages.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = std::string(SYSTEM) + "\nIt is " + now + ".";
    std::lock_guard<std::mutex> g(lock);
    // Only the newest photos travel as images; older ones are mentioned.
    int photos_left = KEEP_PHOTOS;
    // ArduinoJson strings stop at 64 KB, and a photo's data URL is ~100 KB:
    // each goes in as a placeholder, swapped for the URL after serializing.
    std::vector<Jpeg> images;
    std::vector<bool> keep(history.size(), false);
    for (int i = int(history.size()) - 1; i >= 0; --i)
        if (!history[i].photos.empty() && photos_left > 0) {
            keep[i] = true, --photos_left;
        }
    for (size_t i = 0; i < history.size(); ++i) {
        const Msg &m = history[i];
        JsonObject o = messages.add<JsonObject>();
        o["role"] = m.role;
        if (m.role == "tool") {
            o["tool_call_id"] = m.tool_call_id;
        }
        if (!m.tool_calls.empty()) {
            JsonDocument calls;
            deserializeJson(calls, m.tool_calls);
            o["tool_calls"] = calls;
        }
        if (m.photos.empty()) {
            if (m.text.empty() && !m.tool_calls.empty()) {
                o["content"] = nullptr;
            } else {
                o["content"] = m.text;
            }
        } else if (!keep[i]) {
            o["content"] = m.text + (m.text.empty() ? "" : " ") + "[an earlier photo, no longer attached]";
        } else {
            JsonArray parts = o["content"].to<JsonArray>();
            JsonObject text = parts.add<JsonObject>();
            text["type"] = "text";
            text["text"] = m.text.empty() ? "(photo)" : m.text;
            for (const Jpeg &p : m.photos) {
                if (!p || p->size() < 4 || (*p)[0] != 0xFF || (*p)[1] != 0xD8) {
                    continue;    // not a JPEG
                }
                JsonObject img = parts.add<JsonObject>();
                img["type"] = "image_url";
                img["image_url"]["url"] = "@@SWIFTY_IMAGE_" + std::to_string(images.size()) + "@@";
                images.push_back(p);
            }
        }
    }
    std::string body;
    serializeJson(doc, body);
    for (size_t i = 0; i < images.size(); ++i) {
        const std::string mark = "@@SWIFTY_IMAGE_" + std::to_string(i) + "@@";
        const size_t at = body.find(mark);
        if (at != std::string::npos) {
            body.replace(at, mark.size(), data_url(images[i]));
        }
    }
    return body;
}

struct Call {
    std::string id, name, args;
};

// Strips a leading [mood] tag.
Shot take_mood(std::string &text)
{
    static const struct {
        const char *tag;
        Shot shot;
    } MOODS[] = {{"[pleased]", Shot::Pleased}, {"[sorry]", Shot::Sorry}, {"[confused]", Shot::Confused}};
    size_t start = text.find_first_not_of(" \n");
    if (start == std::string::npos) {
        return Shot::Hello;
    }
    for (const auto &m : MOODS)
        if (text.compare(start, strlen(m.tag), m.tag) == 0) {
            text.erase(0, start + strlen(m.tag));
            text.erase(0, text.find_first_not_of(" \n"));
            return m.shot;
        }
    return Shot::Hello;
}

// One streamed completion. Returns false with error set when it failed.
bool complete(const Config &c, const std::string &model_name, const AgentEvents &ev, std::string &text,
              std::vector<Call> &calls, std::string &error)
{
    const std::string body = build_request(model_name);
    esp_http_client_handle_t h = client_for(base_url(c) + "/chat/completions", c, HTTP_METHOD_POST, 90000);
    if (!h) {
        error = "Out of memory.";
        return false;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Accept", "text/event-stream");
    bool ok = esp_http_client_open(h, int(body.size())) == ESP_OK;
    for (size_t at = 0; ok && at < body.size();) {
        const int n = esp_http_client_write(h, body.data() + at, int(std::min<size_t>(4096, body.size() - at)));
        if (n <= 0) {
            ok = false;
        } else {
            at += size_t(n);
        }
    }
    if (!ok || esp_http_client_fetch_headers(h) < 0) {
        esp_http_client_cleanup(h);
        error = "Swifty's model server is unreachable.";
        return false;
    }
    const int status = esp_http_client_get_status_code(h);
    std::string pending, raw;
    char buf[1024];
    int n;
    bool streamed = false, finished = false;
    auto handle_delta = [&](JsonVariantConst choice) {
        JsonVariantConst delta = choice["delta"].isNull() ? choice["message"] : choice["delta"];
        const char *content = delta["content"] | (const char *)nullptr;
        if (content && *content) {
            text += content;
            std::string shown = text;
            take_mood(shown);
            if (ev.text) {
                ev.text(shown);
            }
        }
        for (JsonVariantConst tc : delta["tool_calls"].as<JsonArrayConst>()) {
            const size_t index = tc["index"] | calls.size();
            if (calls.size() <= index) {
                calls.resize(index + 1);
            }
            Call &call = calls[index];
            if (tc["id"].is<const char *>()) {
                call.id = tc["id"].as<const char *>();
            }
            if (tc["function"]["name"].is<const char *>()) {
                call.name += tc["function"]["name"].as<const char *>();
            }
            if (tc["function"]["arguments"].is<const char *>()) {
                call.args += tc["function"]["arguments"].as<const char *>();
            }
        }
    };
    while (!stop && (n = esp_http_client_read(h, buf, sizeof(buf))) > 0) {
        if (status != 200) {
            if (raw.size() < 4096) {
                raw.append(buf, size_t(n));
            }
            continue;
        }
        pending.append(buf, size_t(n));
        if (raw.size() < 262144) {
            raw.append(buf, size_t(n));
        }
        size_t eol;
        while ((eol = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, eol);
            pending.erase(0, eol + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.compare(0, 5, "data:") != 0) {
                continue;
            }
            streamed = true;
            const char *data = line.c_str() + 5;
            while (*data == ' ') {
                ++data;
            }
            if (!strcmp(data, "[DONE]")) {
                finished = true;
                continue;
            }
            JsonDocument chunk;
            if (deserializeJson(chunk, data) == DeserializationError::Ok) {
                handle_delta(chunk["choices"][0]);
            }
        }
    }
    esp_http_client_cleanup(h);
    if (stop) {
        error = "Stopped.";
        return false;
    }
    if (status != 200) {
        JsonDocument doc;
        const char *message = nullptr;
        if (deserializeJson(doc, raw) == DeserializationError::Ok) {
            message = doc["error"]["message"] | (const char *)nullptr;
        }
        error = message ? message : "Swifty's model server refused the request (HTTP " + std::to_string(status) + ").";
        return false;
    }
    if (!streamed) {  // a server that ignored "stream"
        JsonDocument doc;
        if (deserializeJson(doc, raw) != DeserializationError::Ok) {
            error = "Swifty's model server sent an unreadable reply.";
            return false;
        }
        handle_delta(doc["choices"][0]);
    }
    (void)finished;
    return true;
}

// Tavily search: a short answer plus the top results, trimmed for the model.
std::string web_search(const std::string &query)
{
    const Config c = config();
    if (c.search_key.empty()) {
        return "Web search is not set up on this board (no search_key in Swifty's configuration).";
    }
    if (query.empty()) {
        return "Say what to search for.";
    }
    JsonDocument body;
    body["query"] = query;
    body["search_depth"] = "basic";
    body["include_answer"] = true;
    body["max_results"] = 5;
    std::string payload;
    serializeJson(body, payload);
    esp_http_client_config_t cfg = {};
    cfg.url = "https://api.tavily.com/search";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return "Web search could not start.";
    }
    const std::string auth = "Bearer " + c.search_key;
    esp_http_client_set_header(h, "Authorization", auth.c_str());
    esp_http_client_set_header(h, "Content-Type", "application/json");
    std::string reply;
    int status = 0;
    if (esp_http_client_open(h, int(payload.size())) == ESP_OK &&
            esp_http_client_write(h, payload.data(), int(payload.size())) == int(payload.size())) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        char buf[1024];
        for (int n; reply.size() < 48 * 1024 && (n = esp_http_client_read(h, buf, sizeof(buf))) > 0;) {
            reply.append(buf, size_t(n));
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    if (status == 401 || status == 403) {
        return "The web search key was refused.";
    }
    if (status != 200) {
        return "Web search failed" + (status ? " (HTTP " + std::to_string(status) + ")." : std::string("; is the board online?"));
    }
    JsonDocument filter;
    filter["answer"] = true;
    filter["results"][0]["title"] = true;
    filter["results"][0]["url"] = true;
    filter["results"][0]["content"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, reply, DeserializationOption::Filter(filter))) {
        return "Web search sent an unreadable answer.";
    }
    std::string out;
    if (doc["answer"].is<const char *>()) {
        out += std::string("Answer: ") + doc["answer"].as<const char *>() + "\n";
    }
    int i = 0;
    for (JsonObjectConst r : doc["results"].as<JsonArrayConst>()) {
        std::string content = r["content"] | "";
        if (content.size() > 400) {
            content = content.substr(0, 400) + "...";
        }
        out += std::to_string(++i) + ". " + std::string(r["title"] | "") + " (" + std::string(r["url"] | "") + "): " + content + "\n";
    }
    ESP_LOGI(TAG, "web search: %u results", unsigned(i));
    return out.empty() ? "No results." : out;
}

std::string run_tool(const Call &call, const AgentEvents &ev, Jpeg *photo)
{
    JsonDocument args;
    deserializeJson(args, call.args.empty() ? "{}" : call.args);
    if (call.name == "take_photo") {
        if (ev.tool) {
            ev.tool("Taking a photo\xE2\x80\xA6");
        }
        if (ev.camera) {
            ev.camera();
        }
        const int index = camera_app_take_photo(15000);  // a cold start resets and opens the sensor
        if (index < 0) {
            return "The camera did not take a photo (is the camera module in the left slot?).";
        }
        uint8_t *jpeg = nullptr;
        size_t size = 0;
        if (camera_app_photo_jpeg(index, &jpeg, &size)) {
            *photo = std::make_shared<std::vector<uint8_t>>(jpeg, jpeg + size);
            heap_caps_free(jpeg);
        }
        if (ev.show_photo) {
            ev.show_photo(index);
        }
        return "Took photo " + std::to_string(index + 1) + "; it is shown on the screen and attached in the next message.";
    }
    if (call.name == "show_photo") {
        if (ev.camera) {
            ev.camera();
        }
        const int count = camera_app_photo_count();
        if (!count) {
            return "There are no photos yet.";
        }
        int index = args["index"] | 0;
        index = index <= 0 || index > count ? count - 1 : index - 1;
        if (ev.show_photo) {
            ev.show_photo(index);
        }
        return "Showing photo " + std::to_string(index + 1) + " of " + std::to_string(count) + ".";
    }
    if (call.name == "home_status") {
        if (ev.tool) {
            ev.tool("Checking the home\xE2\x80\xA6");
        }
        home_summary_t h;
        home_app_summary(&h);
        if (!h.paired) {
            return "This board is not paired with Swifty Home.";
        }
        std::string out = std::string(h.connected ? "Connected to Swifty Home. " : "Swifty Home is unreachable; last known: ");
        if (h.lights) {
            out += "Lights: " + std::string(h.lights_state) + ". ";
        }
        if (h.music) {
            out += "Music: " + std::string(h.music_state) + ", volume " + std::to_string(h.volume) + ". ";
        }
        if (h.tv) {
            out += std::string(h.tv_name) + ": " + h.tv_state + ". ";
        }
        for (int i = 0; i < h.plug_count; ++i) {
            out += std::string(h.plugs[i].name) + (h.plugs[i].known ? (h.plugs[i].on ? " is on. " : " is off. ") : " is unknown. ");
        }
        return out;
    }
    if (call.name == "web_search") {
        if (ev.tool) {
            ev.tool("Searching the web\xE2\x80\xA6");
        }
        return web_search(args["query"] | "");
    }
    if (call.name == "get_time") {
        char now[64];
        const time_t t = time(nullptr);
        struct tm local;
        localtime_r(&t, &local);
        strftime(now, sizeof(now), "%A %d %B %Y, %H:%M", &local);
        return now;
    }
    swifty_tool_fn fn = nullptr;
    {
        std::lock_guard<std::mutex> g(ext_lock);
        for (const Ext &e : ext_tools)
            if (e.name == call.name) {
                fn = e.fn;
            }
    }
    if (fn) {
        if (ev.tool) {
            ev.tool(("Using " + call.name + "\xE2\x80\xA6").c_str());
        }
        std::string out(4096, '\0');
        fn(call.args.empty() ? "{}" : call.args.c_str(), out.data(), out.size());
        out.resize(strnlen(out.c_str(), out.size()));
        ESP_LOGI(TAG, "tool %s: %u bytes", call.name.c_str(), unsigned(out.size()));
        return out.empty() ? "Done." : out;
    }
    return "Unknown tool " + call.name + ".";
}

void trim_history()
{
    // Drop whole turns from the front, never splitting a tool exchange.
    while (history.size() > MAX_TURNS) {
        size_t cut = 1;
        while (cut < history.size() && history[cut].role != "user") {
            ++cut;
        }
        history.erase(history.begin(), history.begin() + cut);
    }
}

void run(Job &job)
{
    const Config c = config();
    {
        std::lock_guard<std::mutex> g(lock);
        history.push_back({"user", job.text, "", "", job.photos});
        trim_history();
    }
    std::string error;
    const std::string name = c.llm_ready() ? model(c, &error) : "";
    if (!c.llm_ready()) {
        error = "Swifty is not set up yet: load its configuration with tools/mosaico-config.py.";
    }
    std::string text;
    for (int round = 0; error.empty() && round < MAX_ROUNDS; ++round) {
        std::vector<Call> calls;
        text.clear();
        if (!complete(c, name, job.events, text, calls, error)) {
            break;
        }
        calls.erase(std::remove_if(calls.begin(), calls.end(), [](const Call & k) {
            return k.name.empty();
        }), calls.end());
        if (calls.empty()) {
            break;
        }
        // Record the call, run each tool, and go round again.
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (size_t i = 0; i < calls.size(); ++i) {
            if (calls[i].id.empty()) {
                calls[i].id = "call_" + std::to_string(round) + "_" + std::to_string(i);
            }
            JsonObject o = arr.add<JsonObject>();
            o["id"] = calls[i].id;
            o["type"] = "function";
            o["function"]["name"] = calls[i].name;
            o["function"]["arguments"] = calls[i].args.empty() ? "{}" : calls[i].args;
        }
        std::string serialized;
        serializeJson(doc, serialized);
        {
            std::lock_guard<std::mutex> g(lock);
            history.push_back({"assistant", text, serialized, "", {}});
        }
        std::vector<Jpeg> seen;
        for (const Call &call : calls) {
            Jpeg photo;
            const std::string result = run_tool(call, job.events, &photo);
            if (photo) {
                seen.push_back(photo);
            }
            std::lock_guard<std::mutex> g(lock);
            history.push_back({"tool", result, "", call.id, {}});
        }
        if (!seen.empty()) {
            std::lock_guard<std::mutex> g(lock);
            history.push_back({"user", "The photo you just took with the camera.", "", "", seen});
        }
    }
    Shot mood = Shot::Hello;
    std::string shown = text;
    if (error.empty()) {
        mood = take_mood(shown);
        std::lock_guard<std::mutex> g(lock);
        history.push_back({"assistant", text, "", "", {}});
    } else {
        ESP_LOGW(TAG, "reply failed: %s", error.c_str());
        // Keep the question out of the history, so a retry asks it afresh.
        std::lock_guard<std::mutex> g(lock);
        while (!history.empty() && history.back().role != "user") {
            history.pop_back();
        }
        if (!history.empty()) {
            history.pop_back();
        }
    }
    if (job.events.done) {
        job.events.done(error.empty() ? shown : error, mood, !error.empty());
    }
}

void worker(void *)
{
    for (;;) {
        Job *job = nullptr;
        if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE || !job) {
            continue;
        }
        stop = false;
        run(*job);
        delete job;
        busy = false;
    }
}
}  // namespace

void agent_start()
{
    if (jobs) {
        return;
    }
    jobs = xQueueCreate(2, sizeof(Job *));
    // HTTP and JSON only (no flash writes), so the stack can live in PSRAM.
    xTaskCreatePinnedToCoreWithCaps(worker, "swifty-agent", 24 * 1024, nullptr, 4, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool agent_send(const std::string &text, const std::vector<Jpeg> &photos, bool spoken, const AgentEvents &events)
{
    if (!jobs || busy.exchange(true)) {
        return false;
    }
    Job *job = new Job{text, photos, spoken, events};
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        delete job;
        busy = false;
        return false;
    }
    return true;
}

void agent_stop()
{
    stop = true;
}
bool agent_busy()
{
    return busy;
}

void agent_reset()
{
    std::lock_guard<std::mutex> g(lock);
    history.clear();
}

}  // namespace swifty

extern "C" void swifty_app_add_tool(const char *name, const char *description, const char *parameters_json, swifty_tool_fn fn)
{
    using namespace swifty;
    if (!name || !fn) {
        return;
    }
    std::lock_guard<std::mutex> g(ext_lock);
    for (Ext &e : ext_tools)
        if (e.name == name) {
            e = {name, description ? description : "", parameters_json ? parameters_json : "{\"type\":\"object\"}", fn};
            return;
        }
    ext_tools.push_back({name, description ? description : "", parameters_json ? parameters_json : "{\"type\":\"object\"}", fn});
}
