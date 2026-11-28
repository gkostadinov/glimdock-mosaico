// SPDX-License-Identifier: Apache-2.0
// Swifty's Paseo tools (swifty_paseo.h): MCP calls to the daemon's
// /mcp/agents endpoint over HTTP(S), on the task that asks (Swifty's agent
// task, or RPC 64's own); both have PSRAM stacks. One call at a time per task;
// nothing is kept between calls but the last RPC 64 result.
#include "swifty_paseo.h"

#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "ArduinoJson.h"
#include "bsp/esp_mosaico.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "swifty_app.h"
#include "swifty_config.h"
#include "swifty_paseo_proto.h"
#include "swifty_paseo_ui.h"

using namespace swifty;

namespace {
const char *TAG = "swifty-paseo";
constexpr uint16_t RPC_SERVICE = 0x4744, RPC_PASEO = 64;
constexpr size_t MAX_REPLY = 192 * 1024;  // 30 agents are ~40 KB (the daemon sends each list twice)
constexpr int LIST_MS = 15000, ACT_MS = 60000, CONFIRM_MS = 60000;
constexpr int AGENTS = 30, AGENT_HOURS = 24 * 14;
constexpr const char *SUMMARISE = "Tell the person briefly, for the small screen: two or three short sentences.";
std::atomic<int> next_id{1};

std::string host_of(const std::string &url)
{
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    const size_t end = url.find('/', start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// One tools/call. structured gets the answer; error says why it failed.
bool call(const char *tool, JsonVariantConst arguments, int timeout_ms, JsonDocument &structured, std::string &error,
          int *http = nullptr)
{
    const Config c = config();
    if (!c.paseo_ready()) {
        error = "Paseo is not set up on this board: its daemon's address and password go in Web settings, Paseo.";
        return false;
    }
    const std::string url = paseo::endpoint(c.paseo_url);
    const int id = next_id++;
    const std::string body = paseo::call_body(id, tool, arguments);
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    cfg.disable_auto_redirect = true;
    if (url.compare(0, 8, "https://") == 0) {
        if (!c.paseo_ca.empty()) {
            cfg.cert_pem = c.paseo_ca.c_str();
        } else {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }
    const int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        error = "Out of memory.";
        return false;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    // Paseo's endpoint (MCP Streamable HTTP) refuses a client that does not take both.
    esp_http_client_set_header(h, "Accept", "application/json, text/event-stream");
    if (!c.paseo_token.empty()) {
        const std::string auth = "Bearer " + c.paseo_token;
        esp_http_client_set_header(h, "Authorization", auth.c_str());
    }
    bool ok = esp_http_client_open(h, int(body.size())) == ESP_OK;
    for (size_t at = 0; ok && at < body.size();) {
        const int n = esp_http_client_write(h, body.data() + at, int(std::min<size_t>(2048, body.size() - at)));
        ok = n > 0;
        at += ok ? size_t(n) : 0;
    }
    int status = 0;
    std::string reply;
    if (ok && esp_http_client_fetch_headers(h) >= 0) {
        status = esp_http_client_get_status_code(h);
        char buf[1024];
        for (int n; reply.size() < MAX_REPLY && (n = esp_http_client_read(h, buf, sizeof(buf))) > 0;) {
            reply.append(buf, size_t(n));
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    if (http) {
        *http = status;
    }
    ESP_LOGI(TAG, "%s: HTTP %d, %u bytes, %lld ms", tool, status, unsigned(reply.size()),
             (long long)((esp_timer_get_time() - t0) / 1000));
    if (!status) {
        error = "Paseo is unreachable at " + host_of(url) +
                ": is the daemon listening on the board's network (the LAN or the VPN), and is the board online?";
        return false;
    }
    if (status != 200) {
        error = paseo::http_error(status, reply);
        return false;
    }
    std::string text;
    return paseo::parse_reply(reply, id, structured, text, error);
}

JsonDocument args_of(const char *json)
{
    JsonDocument d;
    if (json && *json) {
        deserializeJson(d, json);
    }
    return d;
}

void reply(char *out, size_t capacity, const std::string &text)
{
    const std::string shown = paseo::clip(text, capacity - 1);
    memcpy(out, shown.c_str(), shown.size() + 1);
}

bool ready(char *out, size_t capacity)
{
    if (!swifty_paseo_allowed()) {
        reply(out, capacity, "Paseo is not allowed on this board.");
        return false;
    }
    if (!config().paseo_ready()) {
        reply(out, capacity, "Paseo is not set up on this board: its daemon's address and password go in Web settings, Paseo.");
        return false;
    }
    return true;
}

bool load_agents(std::vector<paseo::Agent> &agents, std::string &error)
{
    JsonDocument a;
    a["limit"] = AGENTS;
    a["sinceHours"] = AGENT_HOURS;
    JsonDocument s;
    if (!call("list_agents", a, LIST_MS, s, error)) {
        return false;
    }
    agents = paseo::agents_from(s);
    return true;
}

// The agent named want, or a reply saying why there is none.
bool find_agent(const std::string &want, paseo::Agent &agent, std::string &why)
{
    std::vector<paseo::Agent> agents;
    if (!load_agents(agents, why)) {
        return false;
    }
    std::string fits;
    const int i = paseo::match_agent(agents, want, &fits);
    if (i >= 0) {
        agent = agents[size_t(i)];
        return true;
    }
    if (!fits.empty()) {
        why = "Several Paseo agents fit \"" + want + "\": " + fits + ". Ask which one.";
        return false;
    }
    std::string names;
    for (size_t k = 0; k < agents.size() && k < 8; ++k) {
        names += (k ? "; " : "") + paseo::agent_name(agents[k]) + " [" + agents[k].short_id + "]";
    }
    why = "No recent Paseo agent is called \"" + want + "\"." + (names.empty() ? "" : " Recent ones: " + names + ".");
    return false;
}

// ---- asking on the screen --------------------------------------------------------------
struct Ask {
    SemaphoreHandle_t done = nullptr;
    std::atomic<int> answer{-1};  // -1 not yet, 0 no, 1 yes
    lv_obj_t *card = nullptr;
};

void answered(bool yes, void *ctx)
{
    Ask *ask = static_cast<Ask *>(ctx);
    int open = -1;
    if (ask->answer.compare_exchange_strong(open, yes ? 1 : 0)) {
        lv_obj_add_flag(ask->card, LV_OBJ_FLAG_HIDDEN);
        xSemaphoreGive(ask->done);
    }
}

// Shows the card and waits up to a minute for a tap. why says what happened
// when the answer is no.
bool confirm(const PaseoAsk &what, std::string &why)
{
    Ask *ask = new Ask;
    ask->done = xSemaphoreCreateBinary();
    if (!ask->done || !bsp_display_lock(3000)) {
        why = "the screen was busy";
        if (ask->done) {
            vSemaphoreDelete(ask->done);
        }
        delete ask;
        return false;
    }
    ask->card = paseo_confirm_card(lv_layer_top(), what, answered, ask);
    bsp_display_unlock();
    ESP_LOGI(TAG, "asking on the screen: %s", what.action.c_str());
    const bool tapped = xSemaphoreTake(ask->done, pdMS_TO_TICKS(CONFIRM_MS)) == pdTRUE;
    // The card goes under the display lock, so no tap reaches it once ask is freed.
    bool locked = false;
    for (int i = 0; i < 20 && !(locked = bsp_display_lock(1000)); ++i) {
    }
    if (!locked) {
        // LVGL is stuck: the card stays, and taps on it do nothing now.
        ESP_LOGW(TAG, "the screen stayed busy; the question is dropped");
        int open = -1;
        ask->answer.compare_exchange_strong(open, 0);
        why = "the screen was busy";
        return false;  // ask stays allocated: the card still points at it
    }
    int open = -1;
    ask->answer.compare_exchange_strong(open, 0);
    lv_obj_delete(ask->card);
    bsp_display_unlock();
    const bool yes = ask->answer == 1;
    why = tapped ? "cancelled on the screen" : "nobody confirmed on the screen within a minute";
    vSemaphoreDelete(ask->done);
    delete ask;
    return yes;
}

// ---- the tools --------------------------------------------------------------------------
void list_agents_tool(const char *a, char *out, size_t capacity)
{
    if (!ready(out, capacity)) {
        return;
    }
    const std::string which = args_of(a)["which"] | "all";
    std::vector<paseo::Agent> agents;
    std::string error;
    if (!load_agents(agents, error)) {
        reply(out, capacity, error);
        return;
    }
    if (which == "working" || which == "needs_you") {
        std::vector<paseo::Agent> some;
        for (const paseo::Agent &x : agents)
            if (which == "needs_you" ? x.needs_you : (x.status == "running" || x.status == "initializing")) {
                some.push_back(x);
            }
        if (some.empty()) {
            reply(out, capacity, which == "needs_you" ? "No Paseo agent needs the person now." : "No Paseo agent is working now.");
            return;
        }
        agents.swap(some);
    }
    reply(out, capacity, paseo::describe_agents(agents, time(nullptr), capacity - 200) + SUMMARISE);
}

void read_tool(const char *a, char *out, size_t capacity)
{
    if (!ready(out, capacity)) {
        return;
    }
    const std::string want = args_of(a)["agent"] | "";
    if (want.empty()) {
        reply(out, capacity, "Say which agent (a name from paseo_list_agents).");
        return;
    }
    paseo::Agent agent;
    std::string why;
    if (!find_agent(want, agent, why)) {
        reply(out, capacity, why);
        return;
    }
    JsonDocument q;
    q["agentId"] = agent.id;
    q["limit"] = 8;
    JsonDocument s;
    if (!call("get_agent_activity", q, LIST_MS, s, why)) {
        reply(out, capacity, why);
        return;
    }
    const std::string head = paseo::agent_line(agent, time(nullptr)) + "\n" + SUMMARISE +
                             " Say what it did or answered last, and whether it needs the person.\nIts latest activity, oldest first:\n";
    const std::string content = s["content"] | "";
    const size_t room = capacity > head.size() + 64 ? capacity - head.size() - 64 : 256;
    reply(out, capacity, head + paseo::activity_tail(content, room));
}

void send_tool(const char *a, char *out, size_t capacity)
{
    if (!ready(out, capacity)) {
        return;
    }
    const JsonDocument d = args_of(a);
    const std::string want = d["agent"] | "", workspace = d["workspace"] | "", message = d["message"] | "";
    const bool interrupt = d["interrupt"] | false;
    if (message.find_first_not_of(" \t\n") == std::string::npos) {
        reply(out, capacity, "Say what to send.");
        return;
    }
    std::string why;
    JsonDocument s;
    if (!want.empty()) {
        paseo::Agent agent;
        if (!find_agent(want, agent, why)) {
            reply(out, capacity, why);
            return;
        }
        const std::string name = paseo::agent_name(agent);
        PaseoAsk ask{"Send to " + name + "?", paseo::agent_state(agent, time(nullptr)), message, "Send"};
        if (agent.status == "running" && interrupt) {
            ask.subtitle = "Stops what it is doing first";
        }
        if (!confirm(ask, why)) {
            reply(out, capacity, "Not sent: " + why + ".");
            return;
        }
        JsonDocument q;
        q["agentId"] = agent.id;
        q["prompt"] = message;
        // A running agent takes the message into its turn unless told to stop.
        q["activeTurnBehavior"] = interrupt ? "interrupt" : "steer";
        if (!call("send_agent_prompt", q, ACT_MS, s, why)) {
            reply(out, capacity, "Not sent: " + why);
            return;
        }
        const std::string now = s["status"] | "";
        reply(out, capacity, "Sent to " + name + (now.empty() ? "" : "; it is " + now + " now") +
              ". paseo_read shows its reply when it is done.");
        return;
    }
    if (workspace.empty()) {
        reply(out, capacity, "Say which agent gets it (paseo_list_agents), or which workspace a new agent starts in (paseo_workspaces).");
        return;
    }
    const std::string provider = config().paseo_provider;
    if (provider.empty()) {
        reply(out, capacity, "Swifty starts no new Paseo agents until a provider for them (paseo_provider, e.g. codex/gpt-5.4) is set in Web settings, Paseo.");
        return;
    }
    if (!call("list_workspaces", JsonVariantConst(), LIST_MS, s, why)) {
        reply(out, capacity, why);
        return;
    }
    const std::vector<paseo::Workspace> spaces = paseo::workspaces_from(s);
    std::string fits;
    const int i = paseo::match_workspace(spaces, workspace, &fits);
    if (i < 0) {
        reply(out, capacity, fits.empty() ? "No Paseo workspace is called \"" + workspace + "\".\n" + paseo::describe_workspaces(spaces, 1500)
              : "Several Paseo workspaces fit \"" + workspace + "\": " + fits + ". Ask which one.");
        return;
    }
    const paseo::Workspace &ws = spaces[size_t(i)];
    const std::string ws_name = ws.title.empty() ? ws.cwd : ws.title;
    if (!confirm({"Start an agent in " + ws_name + "?", provider, message, "Start"}, why)) {
        reply(out, capacity, "Not started: " + why + ".");
        return;
    }
    // Paseo wants a title: the one given, or the message's first line.
    std::string title = d["title"] | "";
    if (title.find_first_not_of(" \t\n") == std::string::npos) {
        const size_t start = message.find_first_not_of(" \t\n");
        title = message.substr(start, message.find('\n', start) - start);
    }
    JsonDocument q;
    q["title"] = paseo::clip(title, 60);
    q["provider"] = provider;
    q["initialPrompt"] = message;
    q["workspaceId"] = ws.id;
    if (!call("create_agent", q, ACT_MS, s, why)) {
        reply(out, capacity, "Not started: " + why);
        return;
    }
    const std::string id = s["agentId"] | "", now = s["status"] | "";
    reply(out, capacity, "Started a new agent in " + ws_name + " [" + id.substr(0, 7) + "]" +
          (now.empty() ? "" : "; it is " + now) + ". paseo_read shows its progress.");
}

void workspaces_tool(const char *, char *out, size_t capacity)
{
    if (!ready(out, capacity)) {
        return;
    }
    JsonDocument s;
    std::string error;
    if (!call("list_workspaces", JsonVariantConst(), LIST_MS, s, error)) {
        reply(out, capacity, error);
        return;
    }
    reply(out, capacity, paseo::describe_workspaces(paseo::workspaces_from(s), capacity - 100));
}

void schedules_tool(const char *a, char *out, size_t capacity)
{
    if (!ready(out, capacity)) {
        return;
    }
    const std::string run = args_of(a)["run"] | "";
    JsonDocument s;
    std::string why;
    if (!call("list_schedules", JsonVariantConst(), LIST_MS, s, why)) {
        reply(out, capacity, why);
        return;
    }
    const std::vector<paseo::Schedule> schedules = paseo::schedules_from(s);
    if (run.empty()) {
        reply(out, capacity, paseo::describe_schedules(schedules, time(nullptr), capacity - 100));
        return;
    }
    std::string fits;
    const int i = paseo::match_schedule(schedules, run, &fits);
    if (i < 0) {
        reply(out, capacity, fits.empty() ? "No Paseo schedule is called \"" + run + "\".\n" +
              paseo::describe_schedules(schedules, time(nullptr), 1500)
              : "Several Paseo schedules fit \"" + run + "\": " + fits + ". Ask which one.");
        return;
    }
    const paseo::Schedule &sc = schedules[size_t(i)];
    const std::string name = sc.name.empty() ? paseo::clip(sc.prompt, 40) : sc.name;
    if (!confirm({"Run " + name + " now?", sc.cadence.empty() ? "A Paseo schedule" : "Normally " + sc.cadence, sc.prompt, "Run"}, why)) {
        reply(out, capacity, "Not run: " + why + ".");
        return;
    }
    JsonDocument q;
    q["id"] = sc.id;
    if (!call("run_schedule_once", q, ACT_MS, s, why)) {
        reply(out, capacity, "Not run: " + why);
        return;
    }
    reply(out, capacity, "Started " + name + " now; it runs as a new agent (paseo_list_agents shows it). Its usual times are unchanged.");
}

// ---- RPC 64 ------------------------------------------------------------------------------
struct {
    std::mutex lock;
    bool busy = false;
    std::string what, text;
    int64_t ms = 0;
} D;

void diag_task(void *arg)
{
    std::string *what = static_cast<std::string *>(arg);
    std::string out(4096, '\0');
    const int64_t t0 = esp_timer_get_time();
    if (*what == "agents") {
        list_agents_tool("{}", out.data(), out.size());
    } else if (*what == "workspaces") {
        workspaces_tool("{}", out.data(), out.size());
    } else if (*what == "schedules") {
        schedules_tool("{}", out.data(), out.size());
    } else {
        JsonDocument a;
        a["agent"] = what->substr(5);
        std::string json;
        serializeJson(a, json);
        read_tool(json.c_str(), out.data(), out.size());
    }
    out.resize(strnlen(out.c_str(), out.size()));
    {
        std::lock_guard<std::mutex> g(D.lock);
        D.what = *what;
        D.text = out;
        D.ms = (esp_timer_get_time() - t0) / 1000;
        D.busy = false;
    }
    delete what;
    vTaskDeleteWithCaps(nullptr);  // its stack is in PSRAM
}

// Payload: "agents", "workspaces", "schedules" or "read NAME" starts a
// read-only call (replies "started", "busy" or why not); none: the last one's
// result as JSON. Nothing here makes an agent act.
esp_err_t rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    std::string reply_text;
    const std::string what(reinterpret_cast<const char *>(request->payload), request->payload_size);
    if (what.empty()) {
        const Config c = config();
        JsonDocument doc;
        doc["configured"] = c.paseo_ready();
        doc["url"] = c.paseo_url;
        doc["token"] = !c.paseo_token.empty();
        doc["allowed"] = swifty_paseo_allowed();
        std::lock_guard<std::mutex> g(D.lock);
        doc["busy"] = D.busy;
        doc["what"] = D.what;
        doc["ms"] = D.ms;
        doc["text"] = D.text.substr(0, capacity > 300 ? capacity - 300 : 0);
        serializeJson(doc, reply_text);
    } else if (what != "agents" && what != "workspaces" && what != "schedules" && what.compare(0, 5, "read ") != 0) {
        reply_text = "unknown: agents, workspaces, schedules or read NAME";
    } else if (!config().paseo_ready()) {
        reply_text = "not configured";
    } else {
        std::lock_guard<std::mutex> g(D.lock);
        if (D.busy) {
            reply_text = "busy";
        } else {
            std::string *arg = new std::string(what);
            // HTTP, TLS and JSON only: a PSRAM stack, below LVGL's draw threads.
            D.busy = xTaskCreatePinnedToCoreWithCaps(diag_task, "paseo-diag", 12 * 1024, arg, 2, nullptr, 0,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
            if (!D.busy) {
                delete arg;
            }
            reply_text = D.busy ? "started" : "out of memory";
        }
    }
    *size = std::min(reply_text.size(), capacity);
    memcpy(out, reply_text.data(), *size);
    return ESP_OK;
}
}  // namespace

extern "C" bool swifty_paseo_allowed(void)
{
    // Profiles: return profiles_can(Capability::paseo);
    return true;
}

extern "C" void swifty_paseo_start(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    swifty_app_add_tool("paseo_list_agents",
                        "The person's Paseo coding agents (on their computers and servers): names, what each is doing "
                        "(working, idle, closed), which need the person (a permission, an error, an unread finish), and when "
                        "each was last active.",
                        R"({"type":"object","properties":{"which":{"type":"string","enum":["all","working","needs_you"]}},"required":[]})",
                        list_agents_tool);
    swifty_app_add_tool("paseo_read",
                        "A Paseo agent's latest reply and progress (its last messages and tool steps). Sum it up in a few "
                        "sentences; never read it out whole.",
                        R"({"type":"object","properties":{"agent":{"type":"string","description":"Its name or short id, from paseo_list_agents"}},"required":["agent"]})",
                        read_tool);
    swifty_app_add_tool("paseo_send",
                        "Give a Paseo agent a message or task (agent), or start a new agent with it in a workspace "
                        "(workspace, from paseo_workspaces). The person confirms on the screen first. A working agent takes "
                        "the message into its current turn unless interrupt is true.",
                        R"({"type":"object","properties":{"agent":{"type":"string"},"workspace":{"type":"string"},)"
                        R"("message":{"type":"string","description":"What the agent gets, in full"},)"
                        R"("title":{"type":"string","description":"A new agent's title, 60 characters at most"},)"
                        R"("interrupt":{"type":"boolean","description":"Stop the agent's current turn first"}},"required":["message"]})",
                        send_tool);
    swifty_app_add_tool("paseo_workspaces", "The person's Paseo workspaces (projects and worktrees), where new agents can start.",
                        R"({"type":"object","properties":{},"required":[]})", workspaces_tool);
    swifty_app_add_tool("paseo_schedules",
                        "The person's Paseo schedules (agents started on a timetable): when each runs next. Or run one now "
                        "(run: its name or id); the person confirms on the screen first.",
                        R"({"type":"object","properties":{"run":{"type":"string"}},"required":[]})", schedules_tool);
    esp_iris_rpc_register(RPC_SERVICE, RPC_PASEO, rpc, nullptr);
}
