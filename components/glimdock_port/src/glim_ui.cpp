// SPDX-License-Identifier: Apache-2.0
// Glimdock on ESP-Mosaico. Layout (480x480 panel, corner radius ~80):
//   top    y 10-40   wordmark, clock, settings
//          y 50-92   system picker and feed status
//   middle y 100-404 the page (scrolls)
//   bottom y 412-462 tab capsule
// Pages are rebuilt from the latest snapshot when it changes, never under a
// finger; the scroll position is kept.
#include "glim_ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Preferences.h"
#include "audio.h"
#include "display_clock.h"
#include "agent_chat.h"
#include "model.h"
#include "operations_model.h"
#ifndef HOMELAB_NATIVE_PREVIEW
#include "iris_serial.h"
#endif
#include "mui.h"
#include "runtime_health.h"

// Clears secrets without the optimizer dropping the write.
static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
    while (n--) {
        *v++ = 0;
    }
}
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#ifndef HOMELAB_STALE_MS
#define HOMELAB_STALE_MS 15000
#endif

const char *homelabPlatformSsid();

namespace {
using namespace mui;
using std::string;

constexpr int PANEL = 480, CONTENT_Y = 100, CONTENT_H = 304, CW = 460, HALF = 224, GAP = 12;
const Accent &ACC = CYAN;

// Standalone chats are gone (Glimdock now keeps chat inside Kordlet cards and
// sessions).
enum Page : uint8_t { SYSTEMS, CLOCK, SETTINGS, OVERVIEW, GUESTS, STORAGE, SENSORS, HEALTH, JOB, TEMPS, AGENTS, NODES, NODE_FORM };
struct TabSet {
    const Page *pages;
    const char *const *labels;
    int count;
};
constexpr Page FLEET_PAGES[] = {SYSTEMS, AGENTS, CLOCK, SETTINGS};
constexpr const char *FLEET_LABELS[] = {"Systems", "Agents", "Clock", "Settings"};
constexpr Page HOST_PAGES[] = {OVERVIEW, GUESTS, STORAGE, SENSORS, HEALTH};
constexpr const char *HOST_LABELS[] = {"Overview", "Guests", "Storage", "Sensors", "Health"};
constexpr Page SERVER_PAGES[] = {OVERVIEW, STORAGE, SENSORS, HEALTH};
constexpr const char *SERVER_LABELS[] = {"Overview", "Storage", "Sensors", "Health"};
constexpr Page PRINTER_PAGES[] = {JOB, TEMPS, HEALTH};
constexpr const char *PRINTER_LABELS[] = {"Job", "Temps", "Health"};

struct {
    bool started = false, active = false, dirty = true, demo = false, fleet = true, sound = true;
    Page page = SYSTEMS;
    const TabSet *tabs = nullptr;
    Snapshot *snap = nullptr;
    NetworkState net;
    ConnectionSettings conn;
    uint64_t sequence = 0;
    uint32_t revision = 0, conn_revision = 0, last_poll = 0, last_clock = 0;
    char node_key[64] = {};
    uint8_t alerts = 0;
    float history[48];
    int history_count = 0;
    char history_node[64] = {};
    lv_obj_t *screen = nullptr, *content = nullptr, *tab_bar = nullptr, *clock = nullptr, *picker = nullptr,
              *status_dot = nullptr, *status = nullptr, *overlay = nullptr;
    // Connection form draft (the editor writes back here).
    string draft[5];
    bool draft_loaded = false;
    lv_obj_t *editor_area = nullptr;
    struct {
        string *target = nullptr;
        void (*back)() = nullptr;
    } edit;
    // Collector nodes (Settings > Collector nodes), in PSRAM.
    NodeConfiguration *cfg = nullptr;
    uint32_t cfg_revision = 0;
    ConfigNode node;
    bool node_existing = false, node_clear_secret = false, node_save_pending = false, node_delete_pending = false,
         node_delete_armed = false;
    string nd[7];  // name, URL, token, ID, poll, timeout, TTL
    string node_message;
    // Agent operations, in PSRAM.
    ops::Snapshot *ops = nullptr;
    ops::Network ops_net;
    uint64_t ops_sequence = 0;
    uint32_t ops_revision = 0;
    int ops_tab = 0;
    // Alert pop-ups: what has been seen, per system.
    uint32_t seen[32];
    int seen_count = 0;
    char seen_node[64] = {};
    bool seen_primed = false;
    struct {
        uint32_t id;
        uint8_t state;
    } node_states[MAX_NODES];
    int node_state_count = -1;
    lv_obj_t *popup = nullptr;
    // A Kordlet card's or session's chat (read only), in PSRAM.
    agentchat::Model *chat = nullptr;
    agentchat::State chat_state;
    uint32_t chat_revision = ~0u;
    lv_obj_t *chat_overlay = nullptr, *chat_body = nullptr;
    char chat_system[65] = {}, chat_id[65] = {}, chat_title[97] = {};
    bool chat_card = true;
    bool chat_requested = false;  // the network took the request (it can be busy for a moment)
    uint32_t popup_at = 0;
    char popup_node[64] = {};
} G;

// ---- formatting -----------------------------------------------------------------
string fmt(const char *f, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}
const char *DASH = "\xE2\x80\x94";
string num(double v, int decimals = 0)
{
    return isfinite(v) ? fmt("%.*f", decimals, v) : string(DASH);
}
string bytes(double b)
{
    if (!isfinite(b) || b < 0) {
        return DASH;
    }
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (b >= 1000 && u < 4) {
        b /= 1000;
        ++u;
    }
    return fmt(b >= 100 || u == 0 ? "%.0f %s" : "%.1f %s", b, units[u]);
}
string gib(double b)
{
    return isfinite(b) ? fmt("%.1f", b / 1073741824.0) : string(DASH);
}
string rate(double b)
{
    return isfinite(b) ? bytes(b) + "/s" : string(DASH);
}
string span(double s)
{
    if (!isfinite(s) || s < 0) {
        return DASH;
    }
    const uint32_t t = uint32_t(s);
    if (t >= 86400) {
        return fmt("%ud %uh", t / 86400, t % 86400 / 3600);
    }
    if (t >= 3600) {
        return fmt("%uh %um", t / 3600, t % 3600 / 60);
    }
    if (t >= 60) {
        return fmt("%um", t / 60);
    }
    return fmt("%us", t);
}
float pct_of(double used, double total)
{
    return total > 0 && isfinite(used) ? float(100.0 * used / total) : NAN;
}
string pct(float v)
{
    return isfinite(v) ? fmt("%d%%", int(lroundf(v))) : string(DASH);
}

// ---- state of the feed -----------------------------------------------------------
bool stale()
{
    return !G.snap->valid || (millis() - G.snap->received) > HOMELAB_STALE_MS;
}
struct Feed {
    const char *text;
    uint32_t color;
};
Feed feed()
{
    if (G.demo || G.snap->demo) return {"DEMO", VIOLET.glow};
    if (!G.net.configured) return {"SETUP", WARN};
    if (!G.net.wifi) return {"OFFLINE", RED};
    if (!G.snap->valid) return {G.net.loading ? "LOADING" : "WAITING", WARN};
    if (stale()) return {"STALE", WARN};
    return {"LIVE", OK};
}
uint8_t node_state(const NodeDescriptor &n)  // 0 healthy, 1 attention, 2 offline
{
    if (!strcmp(n.status, "offline") || !strcmp(n.summary.status, "offline")) {
        return 2;
    }
    const char *s = n.summary.status[0] ? n.summary.status : n.status;
    return !strcmp(s, "healthy") ? 0 : 1;
}
uint32_t state_color(uint8_t s)
{
    return s == 0 ? OK : s == 1 ? WARN : RED;
}
bool is_printer(const NodeDescriptor &n)
{
    return !strcmp(n.type, "klipper");
}
bool is_host(const NodeDescriptor &n)
{
    return strstr(n.type, "proxmox") != nullptr;
}

// ---- small building blocks --------------------------------------------------------
lv_obj_t *C = nullptr;  // the page being built
int Y = 0;

void on(lv_obj_t *o, lv_event_cb_t cb, void *data = nullptr)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
}
lv_obj_t *card(int h, int x = 0, int w = CW, int r = 22)
{
    return glass(C, x, Y, w, h, r, ACC);
}
lv_obj_t *section(const char *title)
{
    lv_obj_t *l = label(C, title, 6, Y, 400, TEXT3, 13, true);
    Y += 24;
    return l;
}
void key(lv_obj_t *p, const char *text, int x, int y, int w = 180)
{
    label(p, text, x, y, w, TEXT3, 13, true);
}
// A big value with its unit on the baseline.
void big(lv_obj_t *p, const string &value, const char *unit, int x, int y, int px, uint32_t color = TEXT)
{
    lv_obj_t *v = label(p, value.c_str(), x, y, 0, color, px, true);
    if (unit && *unit && value != DASH) {
        const int up = std::max(13, px * 2 / 5);
        const int vh = lv_font_get_line_height(font(px, true)), uh = lv_font_get_line_height(font(up, true));
        label(p, unit, x + text_width(value.c_str(), px) + 3, y + vh - uh - px / 9, 0, TEXT2, up, true);
    }
    (void)v;
}
void row_text(lv_obj_t *p, const string &left, const string &right, int y, uint32_t rc = TEXT2)
{
    label(p, left.c_str(), 20, y, 250, TEXT2, 15);
    label(p, right.c_str(), 200, y, 220, rc, 15, true, LV_TEXT_ALIGN_RIGHT);
}
void empty(const char *text)
{
    lv_obj_t *c = card(90);
    label(c, text, 20, 33, 420, TEXT2, 16, false, LV_TEXT_ALIGN_CENTER);
    Y += 100;
}

// ---- history ------------------------------------------------------------------------
void sample_history()
{
    if (strcmp(G.history_node, G.snap->node.id)) {
        strlcpy(G.history_node, G.snap->node.id, sizeof(G.history_node));
        G.history_count = 0;
    }
    const float cpu = G.snap->cpu;
    if (!isfinite(cpu)) {
        return;
    }
    if (G.history_count == 48) {
        memmove(G.history, G.history + 1, sizeof(float) * 47);
    } else {
        ++G.history_count;
    }
    G.history[G.history_count - 1] = cpu;
}

// ---- actions ---------------------------------------------------------------------------
void open_page(Page p);
void rebuild();
void close_overlay();
void nodes_cb(lv_event_t *);
void poll_operations();
bool redirect = false;

// The system the person chose (empty: all systems), until its data arrives.
const NodeDescriptor *chosen()
{
    if (G.fleet) {
        return nullptr;
    }
    if (G.node_key[0]) {
        const int i = nodeIndexForId(*G.snap, G.node_key);
        if (i >= 0) {
            return &G.snap->nodes[i];
        }
    }
    return &G.snap->node;
}
bool loading_node()
{
    return !G.fleet && G.node_key[0] && strcmp(G.snap->node.id, G.node_key);
}

void choose_node(const char *id)
{
    if (!id) {  // all systems
        G.fleet = true;
        G.node_key[0] = 0;
        open_page(SYSTEMS);
        return;
    }
    G.fleet = false;
    strlcpy(G.node_key, id, sizeof(G.node_key));
    const int i = nodeIndexForId(*G.snap, id);
    if (G.demo && i >= 0) {
        G.snap->node = G.snap->nodes[i];
        makeDemo(*G.snap);
    } else {
        requestNodeSelection(id);  // pages show "Loading" until this system's data arrives
    }
    const NodeDescriptor *n = chosen();
    open_page(n && is_printer(*n) ? JOB : OVERVIEW);
}

void node_cb(lv_event_t *e)
{
    const int i = int(intptr_t(lv_event_get_user_data(e)));
    close_overlay();
    choose_node(i < 0 ? nullptr : G.snap->nodes[i].id);
}

void start_demo(lv_event_t *)
{
    G.demo = true;
    makeDemo(*G.snap);
    G.fleet = true;
    open_page(SYSTEMS);
}

void stop_demo(lv_event_t *)
{
    G.demo = false;
    new (G.snap) Snapshot{};
    G.sequence = 0;
    G.fleet = true;
    open_page(SYSTEMS);
}

void goto_settings(lv_event_t *)
{
    G.fleet = true;
    open_page(SETTINGS);
}

// ---- pages -------------------------------------------------------------------------------
void setup_card()
{
    lv_obj_t *c = card(232);
    label(c, "Connect Glimdock", 22, 20, 400, TEXT, 24, true);
    lv_obj_t *t = label(c, "Point it at your collector: Wi-Fi, the collector's snapshot address and a display "
                        "token. Or look around with sample data first.", 22, 58, 416, TEXT2, 16);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    // The firmware's own hint ("Use Configure on this display") is for its
    // old screens; only show other messages (errors) here.
    if (G.net.message[0] && !strstr(G.net.message, "Not configured")) {
        label(c, G.net.message, 22, 128, 416, WARN, 14);
    }
    lv_obj_t *b = fill_button(c, 22, 160, 200, 50, "Set up", ACC, 17);
    on(b, goto_settings);
    lv_obj_t *d = field_button(c, 238, 160, 200, 50, "Preview demo", 17);
    on(d, start_demo);
    Y += 244;
}

void system_card(int i, int x)
{
    const NodeDescriptor &n = G.snap->nodes[i];
    lv_obj_t *c = card(124, x, HALF);
    press_fx(c);
    on(c, node_cb, reinterpret_cast<void *>(intptr_t(i)));
    const uint8_t st = node_state(n);
    dot(c, 18, 22, 10, state_color(st));
    label(c, n.name[0] ? n.name : n.id, 36, 15, 172, TEXT, 17, true);
    label(c, fmt("%s \xC2\xB7 %s", nodeTypeName(n), st == 2 ? "offline" : st == 1 ? "attention" : "healthy").c_str(),
          18, 42, 190, TEXT3, 13);
    const NodeSummary &s = n.summary;
    if (is_printer(n)) {
        big(c, isfinite(s.progress) ? num(s.progress) : string(DASH), "%", 18, 64, 32, WARN);
        label(c, s.printState[0] ? s.printState : "idle", 110, 82, 96, TEXT2, 14, false, LV_TEXT_ALIGN_RIGHT);
    } else {
        big(c, s.available ? num(s.cpu) : string(DASH), "% cpu", 18, 64, 32, ACC.glow);
        if (s.available && isfinite(s.memory)) {
            bar(c, 18, 108, 188, 6, int(s.memory), VIOLET, false);
        }
    }
}

void page_systems()
{
    const Snapshot &s = *G.snap;
    if (!G.demo && !G.net.configured) {
        setup_card();
        return;
    }
    if (!s.valid) {
        empty(G.net.message[0] ? G.net.message : "Waiting for the collector\xE2\x80\xA6");
        return;
    }
    if (!s.nNodes) {  // a single system: its overview is the fleet
        G.fleet = false;
        G.page = OVERVIEW;
        redirect = true;
        return;
    }
    int healthy = 0;
    float cpu = 0, mem = 0;
    int nc = 0, nm = 0;
    for (int i = 0; i < s.nNodes; ++i) {
        const NodeSummary &v = s.nodes[i].summary;
        if (node_state(s.nodes[i]) == 0) {
            ++healthy;
        }
        if (v.available && isfinite(v.cpu)) {
            cpu += v.cpu;
            ++nc;
        }
        if (v.available && isfinite(v.memory)) {
            mem += v.memory;
            ++nm;
        }
    }
    lv_obj_t *sum = card(104);
    big(sum, fmt("%d", healthy), fmt("/ %d", s.nNodes).c_str(), 22, 14, 46, healthy == s.nNodes ? OK : WARN);
    label(sum, healthy == s.nNodes ? "all systems healthy" : "systems healthy", 22, 72, 190, TEXT2, 14);
    key(sum, "FLEET CPU", 220, 18, 110);
    big(sum, nc ? num(cpu / nc) : string(DASH), "%", 220, 36, 32, ACC.glow);
    key(sum, "MEMORY", 340, 18, 110);
    big(sum, nm ? num(mem / nm) : string(DASH), "%", 340, 36, 32, VIOLET.glow);
    Y += 104 + GAP;
    for (int i = 0; i < s.nNodes; i += 2) {
        system_card(i, 0);
        if (i + 1 < s.nNodes) {
            system_card(i + 1, HALF + GAP);
        }
        Y += 124 + GAP;
    }
}

void page_clock()
{
    tm local{};
    uint8_t zone = 0;
    Preferences p;
    if (p.begin("display", true)) {
        zone = p.getUChar("clockZone", 0) % DISPLAY_CLOCK_ZONE_COUNT;
        p.end();
    }
    const bool timed = displayClockTm(time(nullptr), zone, local);
    lv_obj_t *c = card(CONTENT_H - 8);
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December"
                                        };
    label(c, timed ? fmt("%02d:%02d", local.tm_hour, local.tm_min).c_str() : "--:--", 0, 34, 460, TEXT, 112, true,
          LV_TEXT_ALIGN_CENTER);
    label(c, timed ? fmt("%s %d %s", days[local.tm_wday % 7], local.tm_mday, months[local.tm_mon % 12]).c_str()
          : "Waiting for time sync",
          0, 172, 460, TEXT2, 20, true, LV_TEXT_ALIGN_CENTER);
    const Snapshot &s = *G.snap;
    int healthy = 0;
    for (int i = 0; i < s.nNodes; ++i) {
        healthy += node_state(s.nodes[i]) == 0;
    }
    const string line = s.nNodes ? fmt("%d of %d systems healthy", healthy, s.nNodes)
                        : s.valid ? string(s.host) : string("Glimdock");
    dot(c, 230 - text_width(line.c_str(), 16, false) / 2 - 16, 232, 9, s.nNodes && healthy < s.nNodes ? WARN : OK);
    label(c, line.c_str(), 0, 226, 460, TEXT2, 16, false, LV_TEXT_ALIGN_CENTER);
    Y += CONTENT_H;
}

void page_overview()
{
    const Snapshot &s = *G.snap;
    if (!s.valid || loading_node()) {
        empty("Loading this system\xE2\x80\xA6");
        return;
    }
    // CPU with its history, memory with its bar.
    lv_obj_t *cpu = card(150, 0, HALF);
    key(cpu, "CPU", 18, 16);
    big(cpu, num(s.cpu), "%", 18, 34, 46, ACC.glow);
    label(cpu, fmt("%u cores \xC2\xB7 load %s", unsigned(s.nCores ? s.nCores : s.cpuCount), num(s.load[0], 2).c_str()).c_str(),
          18, 94, 196, TEXT3, 13);
    lv_obj_t *sp = spark(cpu, 1, 112, HALF - 2, 36, ACC.glow);
    spark_set(sp, G.history, G.history_count, 100);
    lv_obj_t *mem = card(150, HALF + GAP, HALF);
    const float mp = pct_of(s.memUsed, s.memTotal);
    key(mem, "MEMORY", 18, 16);
    big(mem, num(mp), "%", 18, 34, 46, VIOLET.glow);
    label(mem, fmt("%s / %s GiB", gib(s.memUsed).c_str(), gib(s.memTotal).c_str()).c_str(), 18, 94, 196, TEXT3, 13);
    if (isfinite(mp)) {
        bar(mem, 18, 122, HALF - 36, 8, int(mp), VIOLET);
    }
    Y += 150 + GAP;
    // Temperature, power, network.
    const int w3 = (CW - 2 * GAP) / 3;
    lv_obj_t *t = card(104, 0, w3);
    key(t, "TEMP", 16, 14);
    big(t, num(s.temp), "\xC2\xB0" "C", 16, 32, 32, isfinite(s.temp) && s.temp >= 80 ? RED : TEXT);
    if (isfinite(s.temp)) {
        bar(t, 16, 84, w3 - 32, 6, int(s.temp), s.temp >= 70 ? AMBER : GREEN);
    }
    lv_obj_t *pw = card(104, w3 + GAP, w3);
    key(pw, "POWER", 16, 14);
    big(pw, num(s.watts), "W", 16, 32, 32);
    label(pw, isfinite(s.mhz) ? fmt("%.1f GHz", s.mhz / 1000).c_str() : "", 16, 78, w3 - 32, TEXT3, 13);
    lv_obj_t *nw = card(104, 2 * (w3 + GAP), w3);
    key(nw, "NETWORK", 16, 14);
    label(nw, ("\xE2\x86\x93 " + rate(s.rx)).c_str(), 16, 38, w3 - 28, TEXT, 16, true);
    label(nw, ("\xE2\x86\x91 " + rate(s.tx)).c_str(), 16, 64, w3 - 28, TEXT2, 16, true);
    Y += 104 + GAP;
    // Logical CPUs, one cell each, tinted by use (the original's CPU page).
    if (s.nCores) {
        const int cols = s.nCores <= 4 ? int(s.nCores) : 8, cell_gap = 6, ch = 38;
        const int cw = (CW - 32 - (cols - 1) * cell_gap) / cols, rows = (s.nCores + cols - 1) / cols;
        lv_obj_t *c = card(48 + rows * (ch + cell_gap) + 6);
        key(c, "CPU CORES", 16, 14);
        int busy = 0;
        for (int i = 0; i < s.nCores; ++i) {
            busy += isfinite(s.cores[i]) && s.cores[i] >= 80;
        }
        label(c, busy ? fmt("%d busy of %u", busy, unsigned(s.nCores)).c_str() : fmt("%u logical", unsigned(s.nCores)).c_str(),
              CW - 196, 14, 180, busy ? WARN : TEXT3, 13, true, LV_TEXT_ALIGN_RIGHT);
        for (int i = 0; i < s.nCores; ++i) {
            const float v = s.cores[i];
            const int x = 16 + (i % cols) * (cw + cell_gap), y = 42 + (i / cols) * (ch + cell_gap);
            lv_obj_t *cell = box(c, x, y, cw, ch, 8);
            const int heat = isfinite(v) ? int(28 + std::min(std::max(v, 0.0f), 100.0f) * 1.9f) : 0;
            const uint32_t tint = isfinite(v) && v >= 90 ? AMBER.glow : ACC.glow;
            lv_obj_set_style_bg_color(cell, lv_color_mix(lv_color_hex(tint), lv_color_hex(0x202020), lv_opa_t(heat)), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            const uint32_t ink = heat > 150 ? 0x000000 : TEXT;
            label(cell, isfinite(v) ? num(v).c_str() : "\xE2\x80\x93", 0, 3, cw, ink, 15, true, LV_TEXT_ALIGN_CENTER);
            label(cell, fmt("%d", i).c_str(), 0, 21, cw, heat > 150 ? 0x202020 : TEXT3, 11, false, LV_TEXT_ALIGN_CENTER);
        }
        Y += 48 + rows * (ch + cell_gap) + 6 + GAP;
    }
    // GPUs.
    for (int i = 0; i < s.nGpus; ++i) {
        const Gpu &g = s.gpus[i];
        lv_obj_t *c = card(84);
        label(c, g.name, 20, 14, 300, TEXT, 16, true);
        label(c, fmt("%s \xC2\xB7 %s", g.vendor, g.owner).c_str(), 20, 40, 300, TEXT3, 13);
        big(c, num(g.utilization), "%", 330, 16, 32, ACC.glow);
        if (isfinite(g.utilization)) {
            bar(c, 20, 64, 420, 6, int(g.utilization), ACC, false);
        }
        Y += 84 + GAP;
    }
    // About the system.
    std::vector<std::pair<string, string>> rows;
    rows.push_back({"Uptime", span(s.uptime)});
    if (s.ip[0]) rows.push_back({"Address", s.ip});
    if (s.osRelease[0]) rows.push_back({"System", s.osRelease});
    if (s.architecture[0]) rows.push_back({"Architecture", s.architecture});
    if (isfinite(s.read)) rows.push_back({"Disk I/O", "R " + rate(s.read) + "  W " + rate(s.write)});
    if (s.swapTotal > 0) rows.push_back({"Swap", gib(s.swapUsed) + " / " + gib(s.swapTotal) + " GiB"});
    if (isfinite(s.arc)) rows.push_back({"ZFS ARC", gib(s.arc) + " GiB"});
    if (isfinite(s.batteryPercent))
        rows.push_back({"Battery", pct(float(s.batteryPercent)) + (s.batteryPlugged == 1 ? " \xC2\xB7 charging" : "")});
    lv_obj_t *info = card(20 + 30 * int(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i) {
        row_text(info, rows[i].first, rows[i].second, 14 + 30 * int(i));
    }
    Y += 20 + 30 * int(rows.size()) + GAP;
}

void page_guests()
{
    const Snapshot &s = *G.snap;
    if (!s.valid || loading_node()) {
        empty("Loading this system\xE2\x80\xA6");
        return;
    }
    if (!s.nGuests) {
        empty("No guests on this host.");
        return;
    }
    for (int i = 0; i < s.nGuests; ++i) {
        const Guest &g = s.guests[i];
        const bool running = !strcmp(g.status, "running");
        lv_obj_t *c = card(84);
        dot(c, 20, 21, 10, running ? OK : !strcmp(g.status, "paused") ? WARN : OFF);
        label(c, g.name, 40, 13, 250, TEXT, 17, true);
        label(c, fmt("%s %d \xC2\xB7 %s \xC2\xB7 up %s", !strcmp(g.type, "lxc") ? "LXC" : "VM", g.id, g.status,
                     span(g.uptime).c_str()).c_str(), 40, 40, 260, TEXT3, 13);
        big(c, num(g.cpu), "%", 330, 14, 28, running ? ACC.glow : TEXT3);
        const float mp = pct_of(g.used, g.total);
        if (isfinite(mp)) {
            bar(c, 40, 64, 260, 6, int(mp), VIOLET, false);
        }
        label(c, fmt("%s / %s GiB", gib(g.used).c_str(), gib(g.total).c_str()).c_str(), 310, 58, 130, TEXT3, 13,
              false, LV_TEXT_ALIGN_RIGHT);
        Y += 84 + 10;
    }
}

void page_storage()
{
    const Snapshot &s = *G.snap;
    if (!s.valid || loading_node()) {
        empty("Loading this system\xE2\x80\xA6");
        return;
    }
    if (!s.nPools && !s.nDisks) {
        empty("No storage reported.");
        return;
    }
    if (s.nPools) {
        section("POOLS");
    }
    for (int i = 0; i < s.nPools; ++i) {
        const Pool &p = s.pools[i];
        const float u = pct_of(p.used, p.total);
        lv_obj_t *c = card(86);
        label(c, p.name, 20, 13, 260, TEXT, 17, true);
        label(c, fmt("%s of %s \xC2\xB7 %s", bytes(p.used).c_str(), bytes(p.total).c_str(), p.status).c_str(), 20, 40,
              300, TEXT3, 13);
        big(c, num(u), "%", 350, 12, 28, isfinite(u) && u >= 90 ? RED : isfinite(u) && u >= 75 ? WARN : TEXT);
        if (isfinite(u)) {
            bar(c, 20, 66, 420, 7, int(u), u >= 90 ? AMBER : ACC, false);
        }
        Y += 86 + 10;
    }
    if (s.nDisks) {
        section("DISKS");
    }
    for (int i = 0; i < s.nDisks; ++i) {
        const Disk &d = s.disks[i];
        const bool ok = !strcmp(d.health, "passed") || !strcmp(d.health, "ok") || !strcmp(d.health, "OK");
        lv_obj_t *c = card(72);
        dot(c, 20, 18, 10, d.health[0] ? (ok ? OK : RED) : OFF);
        label(c, d.model[0] ? d.model : d.name, 40, 11, 270, TEXT, 16, true);
        label(c, fmt("%s \xC2\xB7 %s", d.name, d.health[0] ? d.health : "no SMART").c_str(), 40, 38, 270, TEXT3, 13);
        big(c, num(d.temp), "\xC2\xB0" "C", 350, 16, 26, isfinite(d.temp) && d.temp >= 60 ? WARN : TEXT);
        Y += 72 + 10;
    }
}

void page_sensors()
{
    const Snapshot &s = *G.snap;
    if (!s.valid || loading_node()) {
        empty("Loading this system\xE2\x80\xA6");
        return;
    }
    if (!s.nSensors) {
        empty("No sensors reported.");
        return;
    }
    static const char *const kinds[] = {"temperature", "fan", "power", "voltage", "current", nullptr};
    static const char *const titles[] = {"TEMPERATURES", "FANS", "POWER", "VOLTAGES", "CURRENTS", "OTHER"};
    for (int k = 0; k < 6; ++k) {
        std::vector<int> list;
        for (int i = 0; i < s.nSensors; ++i) {
            const char *kind = s.sensors[i].kind;
            bool known = false;
            for (int j = 0; kinds[j]; ++j) {
                known |= !strcmp(kind, kinds[j]);
            }
            if (kinds[k] ? !strcmp(kind, kinds[k]) : !known) {
                list.push_back(i);
            }
        }
        if (list.empty()) {
            continue;
        }
        section(titles[k]);
        lv_obj_t *c = card(14 + 52 * int(list.size()));
        for (size_t j = 0; j < list.size(); ++j) {
            const Sensor &v = s.sensors[list[j]];
            const int y = 12 + 52 * int(j);
            const bool crit = isfinite(v.crit) && v.value >= v.crit, high = isfinite(v.high) && v.value >= v.high;
            label(c, v.name, 20, y, 260, TEXT, 16, true);
            label(c, v.chip, 20, y + 24, 260, TEXT3, 13);
            const bool temp = !strcmp(v.kind, "temperature") || !strcmp(v.unit, "C");
            const string value = !isfinite(v.value) ? string(DASH)
                                 : temp ? fmt("%.0f\xC2\xB0" "C", v.value)
                                 : fmt(fabsf(v.value) < 10 ? "%.2f %s" : "%.0f %s", v.value, v.unit);
            label(c, value.c_str(), 260, y + 6, 180, crit ? RED : high ? WARN : TEXT, 22, true, LV_TEXT_ALIGN_RIGHT);
        }
        Y += 14 + 52 * int(list.size()) + GAP;
    }
}

void page_health()
{
    const Snapshot &s = *G.snap;
    if (!s.valid || loading_node()) {
        empty("Loading this system\xE2\x80\xA6");
        return;
    }
    {
        const NodeDescriptor &n = s.node;
        const uint8_t st = node_state(n);
        lv_obj_t *c = card(104);
        dot(c, 20, 22, 10, state_color(st));
        label(c, st == 0 ? "Healthy" : st == 1 ? "Needs attention" : "Offline", 40, 14, 260, TEXT, 18, true);
        const Feed f = feed();
        label(c, f.text, 300, 16, 140, f.color, 14, true, LV_TEXT_ALIGN_RIGHT);
        row_text(c, "Updated", s.valid ? span((millis() - s.received) / 1000.0) + " ago" : string(DASH), 46);
        row_text(c, "Data sources", fmt("%d", s.nSources), 72);
        Y += 104 + GAP;
    }
    section(s.nAlerts ? "ALERTS" : "ALERTS \xC2\xB7 NONE");
    if (!s.nAlerts) {
        lv_obj_t *c = card(64);
        dot(c, 20, 27, 10, OK);
        label(c, "No active alerts", 40, 20, 380, TEXT, 17, true);
        Y += 64 + GAP;
    }
    for (int i = 0; i < s.nAlerts; ++i) {
        const Alert &a = s.alerts[i];
        const bool critical = !strcmp(a.severity, "critical") || !strcmp(a.severity, "error");
        lv_obj_t *c = card(78);
        dot(c, 20, 20, 10, critical ? RED : WARN);
        label(c, a.severity, 40, 13, 380, critical ? RED : WARN, 13, true);
        lv_obj_t *m = label(c, a.message, 40, 34, 400, TEXT, 15);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
        Y += 78 + 10;
    }
    if (s.nFaults) {
        section("RECENT FAULTS");
        for (int i = 0; i < s.nFaults; ++i) {
            const Fault &f = s.faults[i];
            lv_obj_t *c = card(72);
            label(c, f.kind, 20, 12, 380, WARN, 13, true);
            label(c, f.message, 20, 34, 420, TEXT, 15);
            Y += 72 + 10;
        }
    }
    if (s.nSources) {
        section("SOURCES");
        lv_obj_t *c = card(14 + 40 * s.nSources);
        for (int i = 0; i < s.nSources; ++i) {
            const Source &src = s.sources[i];
            const int y = 12 + 40 * i;
            dot(c, 20, y + 7, 9, !src.enabled ? OFF : src.ok ? OK : RED);
            label(c, src.name, 40, y, 160, TEXT, 15, true);
            label(c, src.ok || !src.error[0] ? ("updated " + span(src.age) + " ago").c_str() : src.error, 200, y + 1,
                  240, src.ok ? TEXT3 : RED, 13, false, LV_TEXT_ALIGN_RIGHT);
        }
        Y += 14 + 40 * s.nSources + GAP;
    }
}

void page_job()
{
    const Printer &p = G.snap->printer;
    if (loading_node()) {
        empty("Loading this printer\xE2\x80\xA6");
        return;
    }
    if (!p.present) {
        empty("Waiting for the printer\xE2\x80\xA6");
        return;
    }
    lv_obj_t *c = card(200);
    key(c, "PRINT JOB", 20, 18);
    label(c, p.state[0] ? p.state : p.klippyState, 240, 16, 200, WARN, 14, true, LV_TEXT_ALIGN_RIGHT);
    big(c, num(p.progress), "%", 20, 40, 64, WARN);
    label(c, p.filename[0] ? p.filename : "No file", 20, 122, 420, TEXT, 16, true);
    if (isfinite(p.progress)) {
        bar(c, 20, 152, 420, 10, int(p.progress), AMBER);
    }
    label(c, fmt("Elapsed %s \xC2\xB7 left %s", span(p.printDuration).c_str(), span(p.remaining).c_str()).c_str(), 20,
          172, 420, TEXT3, 13);
    Y += 200 + GAP;
    for (int i = 0; i < std::min<int>(p.nHeaters, 2); ++i) {
        const Heater &h = p.heaters[i];
        lv_obj_t *hc = card(96, i * (HALF + GAP), HALF);
        key(hc, h.name, 18, 14, 190);
        big(hc, num(h.temp), "\xC2\xB0" "C", 18, 34, 34, isfinite(h.target) && h.target > 0 ? WARN : TEXT);
        if (isfinite(h.target) && h.target > 0) {
            label(hc, fmt("of %.0f", h.target).c_str(), 140, 50, 66, TEXT3, 13, false, LV_TEXT_ALIGN_RIGHT);
        }
    }
    if (p.nHeaters) {
        Y += 96 + GAP;
    }
    if (isfinite(p.currentLayer) && isfinite(p.totalLayers)) {
        lv_obj_t *l = card(64);
        row_text(l, "Layer", fmt("%.0f of %.0f", p.currentLayer, p.totalLayers), 20, TEXT);
        Y += 64 + GAP;
    }
    if (p.message[0]) {
        lv_obj_t *m = card(64);
        label(m, p.message, 20, 20, 420, TEXT2, 15);
        Y += 64 + GAP;
    }
}

void page_temps()
{
    const Printer &p = G.snap->printer;
    if (!p.nHeaters && !p.nTemps) {
        empty("No temperatures reported.");
        return;
    }
    for (int i = 0; i < p.nHeaters; ++i) {
        const Heater &h = p.heaters[i];
        lv_obj_t *c = card(96);
        label(c, h.name, 20, 14, 240, TEXT, 17, true);
        label(c, isfinite(h.target) && h.target > 0 ? fmt("target %.0f\xC2\xB0" "C", h.target).c_str() : "off", 20, 42,
              240, TEXT3, 13);
        big(c, num(h.temp), "\xC2\xB0" "C", 300, 14, 34, isfinite(h.target) && h.target > 0 ? WARN : TEXT);
        if (isfinite(h.temp) && isfinite(h.target) && h.target > 0) {
            bar(c, 20, 72, 420, 7, int(std::min(100.0f, 100 * h.temp / h.target)), AMBER, false);
        }
        Y += 96 + GAP;
    }
    if (p.nTemps) {
        lv_obj_t *c = card(14 + 40 * p.nTemps);
        for (int i = 0; i < p.nTemps; ++i) {
            row_text(c, p.temperatures[i].name, num(p.temperatures[i].temp) + "\xC2\xB0" "C", 14 + 40 * i, TEXT);
        }
        Y += 14 + 40 * p.nTemps + GAP;
    }
}

// ---- settings: connection form and keyboard editor -------------------------------------
const char *const FIELD_TITLES[] = {"Wi-Fi name", "Wi-Fi password", "Collector endpoint", "Display token",
                                    "Setup token"
                                   };
bool secret(int i)
{
    return i == 1 || i == 3 || i == 4;
}

void load_draft()
{
    if (G.draft_loaded) {
        return;
    }
    G.draft_loaded = true;
    G.draft[0] = G.conn.ssid[0] ? G.conn.ssid : homelabPlatformSsid();
    G.draft[2] = G.conn.endpoint;
    G.draft[1] = G.draft[3] = G.draft[4] = "";  // blank keeps what is saved
}

string shown(int i)
{
    if (!secret(i)) {
        return G.draft[i].empty() ? string("Not set") : G.draft[i];
    }
    if (!G.draft[i].empty()) {
        return string(G.draft[i].size(), '*');
    }
    const bool saved = i == 1 ? G.conn.passwordSaved : i == 3 ? G.conn.displayTokenSaved : G.conn.setupTokenSaved;
    return saved ? "Saved" : i == 4 ? "Optional" : "Not set";
}

void open_form();
void editor_done(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY && G.edit.target) {
        *G.edit.target = lv_textarea_get_text(G.editor_area);
    }
    G.edit.target = nullptr;
    close_overlay();
    if (G.edit.back) {
        G.edit.back();
    }
}

// A full-screen text editor with the on-screen keyboard. On Done or Cancel it
// returns through back() (a form re-opens, or a page rebuilds).
void open_editor(string *target, const char *title, uint32_t max, bool secret_value, const char *hint, void (*back)())
{
    close_overlay();
    G.edit.target = target;
    G.edit.back = back;
    lv_obj_t *o = box(G.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    G.overlay = o;
    label(o, title, 156, 34, 220, TEXT, 22, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *ta = lv_textarea_create(o);
    lv_obj_set_pos(ta, 24, 84);
    lv_obj_set_size(ta, 432, 58);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, target->c_str());
    lv_textarea_set_max_length(ta, max);
    lv_textarea_set_password_mode(ta, false);
    (void)secret_value;
    lv_obj_set_style_bg_color(ta, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(ACC.glow), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 18, 0);
    lv_obj_set_style_pad_hor(ta, 16, 0);
    lv_obj_set_style_pad_ver(ta, 15, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TEXT), 0);
    lv_obj_set_style_text_font(ta, font(20, false), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(ACC.glow), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(ta, 2, LV_PART_CURSOR);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    G.editor_area = ta;
    label(o, hint ? hint : "", 40, 152, 400, TEXT3, 14, false, LV_TEXT_ALIGN_CENTER);
    // The shared Mosaico keyboard; addresses and tokens get no automatic capitals.
    mui::KeyboardConfig kc;
    kc.action = "Done";
    kc.accent = &ACC;
    kc.auto_caps = false;
    mui::keyboard(o, ta, 0, 236, PANEL, 234, kc);
    lv_obj_add_event_cb(ta, editor_done, LV_EVENT_READY, nullptr);
    lv_obj_t *cancel = field_button(o, 56, 30, 92, 36, "Cancel", 15, TEXT2);
    lv_obj_add_event_cb(cancel, editor_done, LV_EVENT_CLICKED, nullptr);
}

void field_cb(lv_event_t *e)
{
    const int i = int(intptr_t(lv_event_get_user_data(e)));
    open_editor(&G.draft[i], FIELD_TITLES[i], i == 0 ? 32 : i == 1 ? 64 : i == 2 ? 240 : 192, secret(i),
                secret(i) ? "Leave empty to keep the saved value." : nullptr, open_form);
}
void form_cancel(lv_event_t *)
{
    G.draft_loaded = false;
    close_overlay();
    G.dirty = true;
}
void form_save(lv_event_t *)
{
    ConnectionUpdate u{};
    strlcpy(u.ssid, G.draft[0].c_str(), sizeof(u.ssid));
    strlcpy(u.password, G.draft[1].c_str(), sizeof(u.password));
    strlcpy(u.endpoint, G.draft[2].c_str(), sizeof(u.endpoint));
    strlcpy(u.displayToken, G.draft[3].c_str(), sizeof(u.displayToken));
    strlcpy(u.setupToken, G.draft[4].c_str(), sizeof(u.setupToken));
    if (requestConnectionUpdate(u)) {
        G.demo = false;
        for (auto &d : G.draft) {
            std::fill(d.begin(), d.end(), '\0');
        }
        G.draft_loaded = false;
        close_overlay();
        G.fleet = true;
        open_page(SETTINGS);
    }
    wipe(&u, sizeof(u));
}

void open_form()
{
    close_overlay();
    load_draft();
    lv_obj_t *o = box(G.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    G.overlay = o;
    label(o, "Connection", 40, 24, 400, TEXT, 28, true, LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *f = glass(o, 10, 72 + 64 * i, 460, 56, 22, ACC);
        press_fx(f);
        on(f, field_cb, reinterpret_cast<void *>(intptr_t(i)));
        label(f, FIELD_TITLES[i], 22, 8, 200, TEXT3, 13, true);
        label(f, shown(i).c_str(), 22, 26, 380, TEXT, 16, true);
        icon(f, "chev", 424, 18, 20, TEXT3, 1.6f);
    }
    lv_obj_t *cancel = field_button(o, 60, 400, 170, 50, "Cancel", 17, TEXT2);
    on(cancel, form_cancel);
    lv_obj_t *save = fill_button(o, 250, 400, 170, 50, "Save", ACC, 17);
    on(save, form_save);
    stagger(o, 0, 15);
}

void form_cb(lv_event_t *)
{
    open_form();
}

void sound_cb(lv_event_t *e)
{
    G.sound = toggle_on(lv_event_get_current_target_obj(e));
    boardAudioEnable(G.sound);
    Preferences p;
    if (p.begin("display", false)) {
        p.putBool("sound", G.sound);
        p.end();
    }
    if (G.sound) {
        boardAudioPlay(AudioCue::Test);
    }
}

void page_settings()
{
    const ConnectionSettings &c = G.conn;
    const bool ok = G.net.wifi && G.snap->valid && !stale();
    const int ch = c.message[0] ? 170 : 144;
    lv_obj_t *cc = card(ch);
    dot(cc, 22, 24, 10, ok ? OK : G.net.configured ? WARN : RED);
    label(cc, "Collector", 42, 16, 200, TEXT, 18, true);
    label(cc, ok ? "Connected" : G.net.configured ? "Not answering" : "Not set up", 230, 18, 210,
          ok ? OK : WARN, 15, true, LV_TEXT_ALIGN_RIGHT);
    row_text(cc, "Wi-Fi", c.ssid[0] ? c.ssid : homelabPlatformSsid(), 52);
    row_text(cc, "Endpoint", c.endpoint[0] ? c.endpoint : "Not set", 80);
    row_text(cc, "Display token", c.displayTokenSaved ? "Saved" : "Not set", 108);
    if (c.message[0]) {
        label(cc, c.message, 20, 138, 420, c.saved ? OK : WARN, 13);
    }
    Y += ch + GAP;
    lv_obj_t *edit = fill_button(C, 0, Y, HALF, 52, "Connection", ACC, 17);
    on(edit, form_cb);
    lv_obj_t *nodes = field_button(C, HALF + GAP, Y, HALF, 52, "Collector nodes", 17);
    on(nodes, nodes_cb);
    Y += 52 + GAP;
    lv_obj_t *demo = card(76);
    label(demo, "Sample data", 20, 14, 280, TEXT, 17, true);
    label(demo, G.demo ? "Showing demo systems" : "Preview Glimdock without a collector", 20, 42, 300, TEXT3, 13);
    lv_obj_t *t = toggle(demo, 384, 22, G.demo, ACC);
    lv_obj_add_event_cb(t, [](lv_event_t *e) {
        if (toggle_on(lv_event_get_current_target_obj(e))) {
            start_demo(e);
        } else {
            stop_demo(e);
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    Y += 76 + GAP;
    lv_obj_t *snd = card(76);
    label(snd, "Alert sounds", 20, 14, 280, TEXT, 17, true);
    label(snd, "A chime when a new alert appears", 20, 42, 300, TEXT3, 13);
    lv_obj_t *st = toggle(snd, 384, 22, G.sound, ACC);
    lv_obj_add_event_cb(st, sound_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    Y += 76 + GAP;
}

// ---- collector nodes (Settings > Collector nodes) --------------------------------------------
void back_to_settings(lv_event_t *)
{
    open_page(SETTINGS);
}
void back_to_nodes(lv_event_t *)
{
    open_page(NODES);
}
void rebuild_later()
{
    G.dirty = true;
}

void page_header(const char *title, const char *detail, lv_event_cb_t back)
{
    lv_obj_t *b = field_button(C, 0, Y, 92, 40, "Back", 15, TEXT2);
    on(b, back);
    label(C, title, 106, Y + 2, 240, TEXT, 20, true);
    if (detail) {
        label(C, detail, 106, Y + 26, 340, TEXT3, 13);
    }
    Y += 52;
}

void note_card(const string &text, uint32_t color = TEXT2)
{
    lv_obj_t *c = card(10);
    lv_obj_t *l = label(c, text.c_str(), 20, 14, 420, color, 15);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(l, LV_SIZE_CONTENT);
    lv_obj_update_layout(l);
    const int h = lv_obj_get_height(l) + 28;
    lv_obj_set_height(c, h);
    Y += h + GAP;
}

string node_kind(const ConfigNode &n)
{
    if (deviceNodeType(n.type)) {
        return string(platformName(n.platform)) + (localNodeType(n.type) ? " \xC2\xB7 local" : agentNodeType(n.type) ? " \xC2\xB7 agent" : " \xC2\xB7 feed");
    }
    return !strcmp(n.type, "local-proxmox") ? "Local Proxmox" : !strcmp(n.type, "klipper") ? "Klipper" : "Remote Proxmox feed";
}

void refresh_nodes_cb(lv_event_t *)
{
    requestNodeConfiguration();
}

void begin_node(const ConfigNode &n, bool existing)
{
    G.node = n;
    G.node_existing = existing;
    G.node_clear_secret = false;
    G.node_delete_armed = false;
    G.nd[0] = n.name;
    G.nd[1] = n.url;
    G.nd[2] = "";
    G.nd[3] = existing ? n.id : "";
    G.nd[4] = fmt("%g", n.poll);
    G.nd[5] = fmt("%g", n.timeout);
    G.nd[6] = fmt("%g", n.ttl);
    G.node_message.clear();
    open_page(NODE_FORM);
}

void edit_node_cb(lv_event_t *e)
{
    const int i = int(intptr_t(lv_event_get_user_data(e)));
    if (i >= 0 && i < G.cfg->count) {
        begin_node(G.cfg->nodes[i], true);
    }
}

void add_node_cb(lv_event_t *)
{
    ConfigNode n;
    strlcpy(n.type, "server-feed", sizeof(n.type));
    n.poll = 5;
    n.timeout = 3;
    n.ttl = 15;
    begin_node(n, false);
}

void page_nodes()
{
    const NodeConfiguration &c = *G.cfg;
    page_header("Collector nodes",
                c.available ? fmt("%d active \xC2\xB7 %d saved", activeConfiguredNodes(c), c.count).c_str() : nullptr,
                back_to_settings);
    if (c.message[0]) {
        note_card(c.message, c.busy ? ACC.glow : WARN);
    }
    if (!c.available) {
        lv_obj_t *b = fill_button(C, 0, Y, CW, 52, c.busy ? "Loading\xE2\x80\xA6" : "Load collector settings", ACC, 17);
        on(b, refresh_nodes_cb);
        Y += 52 + GAP;
        return;
    }
    note_card("Pair new device agents in the collector's web console. Polling feeds stay available here for "
              "printers and APIs.", TEXT3);
    for (int i = 0; i < c.count; ++i) {
        const ConfigNode &n = c.nodes[i];
        lv_obj_t *r = card(78);
        press_fx(r);
        on(r, edit_node_cb, reinterpret_cast<void *>(intptr_t(i)));
        dot(r, 20, 20, 10, n.enabled ? OK : OFF);
        label(r, n.name[0] ? n.name : n.id, 40, 12, 360, TEXT, 17, true);
        label(r, (node_kind(n) + (n.enabled ? "" : " \xC2\xB7 paused")).c_str(), 40, 38, 360, TEXT2, 13);
        label(r, n.id, 40, 56, 360, TEXT3, 12);
        icon(r, "chev", 420, 28, 20, TEXT3, 1.6f);
        Y += 78 + 10;
    }
    if (!c.count) {
        note_card("No nodes yet. Add a monitor below.");
    }
    const bool room = c.count < MAX_CONFIG_NODES && activeConfiguredNodes(c) < MAX_NODES && activeConfiguredRoots(c) < 4;
    if (room) {
        lv_obj_t *b = fill_button(C, 0, Y, CW, 52, "Add node", ACC, 17);
        on(b, add_node_cb);
        Y += 52 + GAP;
    } else {
        note_card("The collector is at its node limit.", WARN);
    }
}

void node_type_cb(lv_event_t *e)
{
    if (G.node_existing) {
        return;
    }
    const int sel = capsule_selected(lv_event_get_current_target_obj(e));
    const NodeConfiguration &c = *G.cfg;
    if (sel == 0 && c.localEnabled) {
        G.node_message = "The local node is already configured.";
        rebuild_later();
        return;
    }
    strlcpy(G.node.type, sel == 0 ? (!strcmp(c.localType, "server") ? "local-server" : "local-proxmox")
            : sel == 1 ? "klipper" : sel == 2 ? "proxmox-feed" : "server-feed", sizeof(G.node.type));
    G.node.platform[0] = 0;
    if (sel == 0) {
        strlcpy(G.node.platform, c.localPlatform, sizeof(G.node.platform));
        strlcpy(G.node.id, c.localId, sizeof(G.node.id));
        G.nd[0] = c.localName;
        G.nd[3] = c.localId;
    } else {
        G.node.id[0] = 0;
        G.nd[3] = "";
    }
    G.node_message.clear();
    rebuild_later();
}

void node_local_mode_cb(lv_event_t *e)
{
    const bool server = capsule_selected(lv_event_get_current_target_obj(e)) == 0;
    const NodeConfiguration &c = *G.cfg;
    if (!server && c.localPlatform[0] && strcmp(c.localPlatform, "linux")) {
        G.node_message = "Proxmox collection requires a Linux host.";
        rebuild_later();
        return;
    }
    strlcpy(G.node.type, server ? "local-server" : "local-proxmox", sizeof(G.node.type));
    strlcpy(G.node.platform, server ? c.localPlatform : "linux", sizeof(G.node.platform));
    rebuild_later();
}

void node_platform_cb(lv_event_t *)
{
    static const char *const values[] = {"", "linux", "macos", "windows", "router", "other"};
    int next = 0;
    for (int i = 0; i < 6; ++i)
        if (!strcmp(G.node.platform, values[i])) {
            next = (i + 1) % 6;
        }
    strlcpy(G.node.platform, values[next], sizeof(G.node.platform));
    rebuild_later();
}

void back_to_node_form()
{
    G.page = NODE_FORM;
    G.dirty = true;
}

const char *const NODE_FIELDS[] = {"Node name", "Service URL", "Service token", "ID (optional slug)",
                                   "Poll interval (seconds)", "Timeout (seconds)", "Freshness TTL (seconds)"
                                  };
void node_field_cb(lv_event_t *e)
{
    const int i = int(intptr_t(lv_event_get_user_data(e)));
    static const uint32_t limits[] = {63, 240, 256, 32, 8, 8, 8};
    open_editor(&G.nd[i], NODE_FIELDS[i], limits[i], i == 2,
                i == 2 ? (G.node_existing ? "Leave empty to keep the saved token." : "Optional.") : nullptr,
                back_to_node_form);
}

double draft_number(const string &v)
{
    char *end = nullptr;
    const double r = strtod(v.c_str(), &end);
    return end != v.c_str() && !*end && isfinite(r) ? r : NAN;
}

void node_save_cb(lv_event_t *)
{
    const NodeConfiguration &c = *G.cfg;
    if (!c.available) {
        G.node_message = "Load collector settings before saving.";
        rebuild_later();
        return;
    }
    NodeConfigDraft d;
    d.node = G.node;
    if (localNodeType(d.node.type)) {
        strlcpy(d.node.platform, deviceNodeType(d.node.type) ? c.localPlatform : "linux", sizeof(d.node.platform));
    }
    strlcpy(d.node.name, G.nd[0].c_str(), sizeof(d.node.name));
    strlcpy(d.node.url, G.nd[1].c_str(), sizeof(d.node.url));
    if (!G.node_existing && !localNodeType(d.node.type)) {
        strlcpy(d.node.id, G.nd[3].c_str(), sizeof(d.node.id));
    }
    strlcpy(d.secret, G.nd[2].c_str(), sizeof(d.secret));
    d.clearSecret = G.node_clear_secret;
    d.node.poll = draft_number(G.nd[4]);
    d.node.timeout = draft_number(G.nd[5]);
    d.node.ttl = draft_number(G.nd[6]);
    if (!localNodeType(d.node.type) && (!isfinite(d.node.poll) || (!agentNodeType(d.node.type) && !isfinite(d.node.timeout)) ||
                                        !isfinite(d.node.ttl))) {
        G.node_message = "Enter valid polling, timeout and TTL numbers.";
    } else if (requestNodeUpsert(d, c.version)) {
        G.node_save_pending = true;
        G.node_message.clear();
    } else {
        G.node_message = "Check the node ID, or wait for the active settings request.";
    }
    wipe(d.secret, sizeof(d.secret));
    rebuild_later();
}

void node_delete_cb(lv_event_t *)
{
    if (!G.node_delete_armed) {
        G.node_delete_armed = true;
        rebuild_later();
        return;
    }
    if (requestNodeDelete(G.node.id, G.cfg->version)) {
        G.node_delete_pending = true;
        G.node_message.clear();
    } else {
        G.node_message = "Wait for the active settings request, then retry.";
    }
    G.node_delete_armed = false;
    rebuild_later();
}

void form_row(const char *title, const string &value, int field, bool enabled = true)
{
    lv_obj_t *f = card(60);
    label(f, title, 20, 9, 380, TEXT3, 13, true);
    label(f, value.c_str(), 20, 29, 380, enabled ? TEXT : TEXT3, 16, true);
    if (enabled) {
        press_fx(f);
        on(f, node_field_cb, reinterpret_cast<void *>(intptr_t(field)));
        icon(f, "chev", 420, 20, 20, TEXT3, 1.6f);
    }
    Y += 60 + 8;
}

void page_node_form()
{
    ConfigNode &n = G.node;
    const NodeConfiguration &c = *G.cfg;
    const bool local = localNodeType(n.type);
    page_header(G.node_existing ? "Edit node" : "New node", G.node_existing ? n.id : nullptr, back_to_nodes);
    if (!G.node_existing) {
        static const char *const types[] = {"Local", "Klipper", "Proxmox", "Server"};
        const int sel = local ? 0 : !strcmp(n.type, "klipper") ? 1 : !strcmp(n.type, "proxmox-feed") ? 2 : 3;
        lv_obj_t *t = capsule(C, 0, Y, CW, 48, types, 4, sel, ACC, 15);
        lv_obj_add_event_cb(t, node_type_cb, LV_EVENT_VALUE_CHANGED, nullptr);
        Y += 48 + GAP;
    } else {
        lv_obj_t *k = card(48);
        label(k, node_kind(n).c_str(), 20, 13, 420, TEXT2, 16, true);
        Y += 48 + GAP;
    }
    if (local) {
        static const char *const modes[] = {"Server", "Proxmox"};
        lv_obj_t *m = capsule(C, 0, Y, CW, 44, modes, 2, !strcmp(n.type, "local-server") ? 0 : 1, ACC, 15);
        lv_obj_add_event_cb(m, node_local_mode_cb, LV_EVENT_VALUE_CHANGED, nullptr);
        Y += 44 + GAP;
    } else if (deviceNodeType(n.type)) {
        lv_obj_t *pf = card(60);
        press_fx(pf);
        on(pf, node_platform_cb);
        label(pf, "Platform (tap to change)", 20, 9, 380, TEXT3, 13, true);
        label(pf, n.platform[0] ? platformName(n.platform) : "Any", 20, 29, 380, TEXT, 16, true);
        Y += 60 + 8;
    }
    form_row(NODE_FIELDS[0], G.nd[0].empty() ? "Not set" : G.nd[0], 0);
    if (!local) {
        form_row(NODE_FIELDS[1], G.nd[1].empty() ? "Not set" : G.nd[1], 1);
        const string token = G.node_clear_secret ? "Will be cleared" : !G.nd[2].empty() ? string(G.nd[2].size(), '*')
                             : n.hasSecret ? "Saved" : "Not set";
        form_row(NODE_FIELDS[2], token, 2, !G.node_clear_secret);
        if (G.node_existing && n.hasSecret) {
            lv_obj_t *cl = card(56);
            label(cl, "Clear the saved token", 20, 17, 300, TEXT, 16, true);
            lv_obj_t *t = toggle(cl, 384, 12, G.node_clear_secret, ACC);
            lv_obj_add_event_cb(t, [](lv_event_t *e) {
                G.node_clear_secret = toggle_on(lv_event_get_current_target_obj(e));
                if (G.node_clear_secret) {
                    G.nd[2].clear();
                }
                rebuild_later();
            }, LV_EVENT_VALUE_CHANGED, nullptr);
            Y += 56 + 8;
        }
        form_row(NODE_FIELDS[3], G.node_existing ? string(n.id) : G.nd[3].empty() ? "From the name" : G.nd[3], 3,
                 !G.node_existing);
        form_row(NODE_FIELDS[4], G.nd[4], 4);
        if (!agentNodeType(n.type)) {
            form_row(NODE_FIELDS[5], G.nd[5], 5);
        }
        form_row(NODE_FIELDS[6], G.nd[6], 6);
    }
    lv_obj_t *en = card(56);
    label(en, "Monitoring enabled", 20, 17, 300, TEXT, 16, true);
    lv_obj_t *t = toggle(en, 384, 12, n.enabled, ACC);
    lv_obj_add_event_cb(t, [](lv_event_t *e) {
        G.node.enabled = toggle_on(lv_event_get_current_target_obj(e));
    },
    LV_EVENT_VALUE_CHANGED, nullptr);
    Y += 56 + GAP;
    const string message = !G.node_message.empty() ? G.node_message : (c.busy || c.message[0]) ? string(c.message) : string();
    if (!message.empty()) {
        note_card(message, c.busy ? ACC.glow : WARN);
    }
    lv_obj_t *save = fill_button(C, 0, Y, G.node_existing ? HALF : CW, 52, c.busy ? "Saving\xE2\x80\xA6" : "Save", ACC, 17);
    on(save, node_save_cb);
    if (G.node_existing) {
        lv_obj_t *del = field_button(C, HALF + GAP, Y, HALF, 52, G.node_delete_armed ? "Tap to confirm" : "Delete", 17, RED);
        on(del, node_delete_cb);
    }
    Y += 52 + GAP;
}

void poll_nodes()
{
    if (!readNodeConfiguration(*G.cfg) || G.cfg->revision == G.cfg_revision) {
        return;
    }
    G.cfg_revision = G.cfg->revision;
    if ((G.node_save_pending || G.node_delete_pending) && !G.cfg->busy) {
        const bool ok = G.cfg->saved || G.cfg->applying;
        if (ok) {
            G.node_save_pending = G.node_delete_pending = false;
            G.page = NODES;
        } else {
            G.node_save_pending = G.node_delete_pending = false;
            G.node_message = G.cfg->message;
        }
    }
    if (G.page == NODES || G.page == NODE_FORM) {
        G.dirty = true;
    }
}

void nodes_cb(lv_event_t *)
{
    requestNodeConfiguration();
    open_page(NODES);
}

// ---- agent operations ------------------------------------------------------------------------
double ops_age(double sampled)
{
    const ops::Snapshot &o = *G.ops;
    if (!o.valid || !isfinite(sampled)) {
        return NAN;
    }
    const double elapsed = uint32_t(millis() - o.received) / 1000.0;
    double now = double(time(nullptr));
    if (now < 1700000000) {
        now = o.generated + elapsed;
    }
    return sampled > now + 30 ? NAN : std::max(0.0, now - sampled);
}
uint32_t ops_color(const char *state)
{
    if (!strcmp(state, "failed") || !strcmp(state, "error")) {
        return RED;
    }
    if (!strcmp(state, "blocked") || !strcmp(state, "waiting") || !strcmp(state, "degraded")) {
        return WARN;
    }
    if (!strcmp(state, "running") || !strcmp(state, "healthy") || !strcmp(state, "ready") || !strcmp(state, "active") ||
            !strcmp(state, "completed") || !strcmp(state, "done")) {
        return OK;
    }
    return TEXT3;
}
string ago(double sampled)
{
    const double a = ops_age(sampled);
    return isfinite(a) ? span(a) + " ago" : string("age unknown");
}
const char *system_name(uint8_t i)
{
    return i < G.ops->nSystems ? G.ops->systems[i].name : "";
}

void demo_operations(ops::Snapshot &o)
{
    new (&o) ops::Snapshot{};
    o.valid = o.demo = true;
    o.generated = double(time(nullptr));
    o.received = millis();
    o.sequence = millis() / 1000;
    o.nSystems = 2;
    const char *names[] = {"Build farm", "Research"}, *providers[] = {"codex", "claude"};
    for (int i = 0; i < 2; ++i) {
        ops::System &s = o.systems[i];
        strlcpy(s.id, i ? "research" : "build", sizeof(s.id));
        strlcpy(s.name, names[i], sizeof(s.name));
        strlcpy(s.provider, providers[i], sizeof(s.provider));
        strlcpy(s.status, "healthy", sizeof(s.status));
        s.sampled = o.generated;
        s.ttl = 60;
        s.running = i ? 1 : 2;
        s.queued = i ? 0 : 3;
        s.blocked = i ? 1 : 0;
        s.failed = 0;
        s.usage.cost = i ? 2.4 : 6.1;
        strlcpy(s.usage.currency, "USD", sizeof(s.usage.currency));
    }
    // The build farm is a Kordlet board.
    o.systems[0].board = true;
    const double lanes[ops::LANES] = {4, 1, 2, 1, 1, 9};
    for (int l = 0; l < ops::LANES; ++l) {
        o.systems[0].lanes[l] = lanes[l];
    }
    o.systems[0].needsYou = 1;
    o.systems[0].cardsTotal = 18;
    const char *agents[] = {"builder-1", "builder-2", "reviewer", "scholar"};
    const char *tasks[] = {"Port Glimdock to Mosaico", "Fix flaky CI job", "Review AMOLED kit", "Summarise papers"};
    o.nAgents = o.nTasks = 4;
    for (int i = 0; i < 4; ++i) {
        ops::Agent &a = o.agents[i];
        a.system = i < 3 ? 0 : 1;
        strlcpy(a.id, agents[i], sizeof(a.id));
        strlcpy(a.name, agents[i], sizeof(a.name));
        strlcpy(a.provider, providers[a.system], sizeof(a.provider));
        strlcpy(a.model, i < 3 ? "gpt-5" : "opus", sizeof(a.model));
        strlcpy(a.state, i == 2 ? "waiting" : "running", sizeof(a.state));
        strlcpy(a.task, tasks[i], sizeof(a.task));
        a.heartbeat = o.generated - 4 - i * 3;
        a.ttl = 120;
        ops::Task &t = o.tasks[i];
        t.system = a.system;
        snprintf(t.id, sizeof(t.id), "t%d", i);
        strlcpy(t.title, tasks[i], sizeof(t.title));
        strlcpy(t.state, i == 1 ? "blocked" : i == 2 ? "queued" : "running", sizeof(t.state));
        strlcpy(t.agent, agents[i], sizeof(t.agent));
        t.progress = i == 0 ? 72 : i == 3 ? 35 : NAN;
        t.updated = o.generated - 30 * i;
        if (i < 3) {  // Kordlet cards and their sessions
            strlcpy(t.kind, "card", sizeof(t.kind));
            t.lane = int8_t(i == 0 ? 2 : i == 1 ? 3 : 4);
            strlcpy(t.status, i == 2 ? "awaiting review" : "in progress", sizeof(t.status));
            if (i == 2) {
                strlcpy(t.attention, "Review the AMOLED kit changes", sizeof(t.attention));
                strlcpy(t.attentionLevel, "warn", sizeof(t.attentionLevel));
            }
            t.chat = true;
            strlcpy(a.kind, "session", sizeof(a.kind));
            strlcpy(a.stage, i == 2 ? "review" : "implement", sizeof(a.stage));
            strlcpy(a.activity, i == 0 ? "Editing glim_ui.cpp" : i == 1 ? "Running the test suite" : "Waiting for you",
                    sizeof(a.activity));
            a.chat = true;
        }
    }
    o.nEvents = 3;
    const char *events[] = {"Tests passed", "CI job blocked on review", "New task queued"};
    const char *severity[] = {"info", "warning", "info"};
    for (int i = 0; i < 3; ++i) {
        ops::Event &e = o.events[i];
        e.system = 0;
        snprintf(e.id, sizeof(e.id), "e%d", i);
        strlcpy(e.title, events[i], sizeof(e.title));
        strlcpy(e.kind, i == 1 ? "task" : "build", sizeof(e.kind));
        strlcpy(e.severity, severity[i], sizeof(e.severity));
        e.at = o.generated - 60 * (i + 1);
    }
}

// Kordlet lane colours (the firmware's laneColor).
uint32_t lane_color(int lane)
{
    return lane == 5 ? OK : lane == 4 ? WARN : lane == 2 || lane == 3 ? ACC.glow : lane == 1 ? VIOLET.glow : TEXT3;
}
uint32_t attention_color(const char *level)
{
    return !strcmp(level, "error") ? RED : !strcmp(level, "info") ? ACC.glow : WARN;
}
const char *lane_label(int lane)
{
    return lane >= 0 && lane < ops::LANES ? ops::LANE_LABELS[lane] : "No lane";
}

void open_chat(const char *system, const char *id, bool card, const char *title);

void card_tap_cb(lv_event_t *e)
{
    const int i = int(intptr_t(lv_event_get_user_data(e)));
    const ops::Snapshot &o = *G.ops;
    if (i < 0 || i >= o.nTasks || !o.tasks[i].chat || o.tasks[i].system >= o.nSystems) {
        return;
    }
    open_chat(o.systems[o.tasks[i].system].id, o.tasks[i].id, true, o.tasks[i].title);
}

void session_tap_cb(lv_event_t *e)
{
    const int i = int(intptr_t(lv_event_get_user_data(e)));
    const ops::Snapshot &o = *G.ops;
    if (i < 0 || i >= o.nAgents || !o.agents[i].chat || o.agents[i].system >= o.nSystems) {
        return;
    }
    open_chat(o.systems[o.agents[i].system].id, o.agents[i].id, false, o.agents[i].name);
}

// One Kordlet system: who needs you, and the six board lanes.
void kordlet_board(const ops::System &s, bool fresh)
{
    lv_obj_t *c = card(196);
    dot(c, 20, 22, 10, fresh ? ops_color(s.status) : WARN);
    label(c, s.name, 40, 14, 260, TEXT, 17, true);
    label(c, fresh ? fmt("%s cards", num(s.cardsTotal).c_str()).c_str() : "stale", 300, 16, 140, TEXT3, 13, true,
          LV_TEXT_ALIGN_RIGHT);
    lv_obj_t *needs = box(c, 16, 44, 132, 136, 18);
    lv_obj_set_style_bg_color(needs, lv_color_hex(0x2A2210), 0);
    lv_obj_set_style_bg_opa(needs, LV_OPA_COVER, 0);
    key(needs, "NEEDS YOU", 14, 14, 110);
    const double n = fresh ? s.needsYou : NAN;
    big(needs, num(n), nullptr, 14, 40, 52, isfinite(n) && n > 0 ? WARN : TEXT);
    label(needs, "cards waiting", 14, 104, 110, TEXT3, 12);
    for (int l = 0; l < ops::LANES; ++l) {
        lv_obj_t *t = box(c, 156 + (l % 3) * 96, 44 + (l / 3) * 70, 90, 64, 14);
        lv_obj_set_style_bg_color(t, lv_color_hex(FIELD), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        label(t, ops::LANE_LABELS[l], 10, 8, 76, lane_color(l), 12, true);
        label(t, num(fresh ? s.lanes[l] : NAN).c_str(), 10, 26, 76, TEXT, 24, true);
    }
    Y += 196 + 10;
}

void ops_tab_cb(lv_event_t *e)
{
    G.ops_tab = capsule_selected(lv_event_get_current_target_obj(e));
    rebuild_later();
}

void page_agents()
{
    const ops::Snapshot &o = *G.ops;
    if (!o.valid) {
        lv_obj_t *c = card(150);
        icon(c, "tech", 20, 18, 28, ACC.glow, 1.8f);
        label(c, "Agent operations", 60, 20, 360, TEXT, 18, true);
        // Before the first answer the network's default is a setup hint: say it is loading.
        const bool waiting = !G.ops_net.revision && !G.ops_net.connected;
        lv_obj_t *m = label(c, waiting ? "Loading agent operations\xE2\x80\xA6" : G.ops_net.message, 20, 62, 420, TEXT2, 15);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_height(m, 70);
        Y += 150 + GAP;
        return;
    }
    double running = 0, queued = 0, blocked = 0, failed = 0, cost = 0;
    const char *currency = "";
    for (int i = 0; i < o.nSystems; ++i) {
        const ops::System &s = o.systems[i];
        if (isfinite(s.running)) {
            running += s.running;
        }
        if (isfinite(s.queued)) {
            queued += s.queued;
        }
        if (isfinite(s.blocked)) {
            blocked += s.blocked;
        }
        if (isfinite(s.failed)) {
            failed += s.failed;
        }
        if (isfinite(s.usage.cost)) {
            cost += s.usage.cost;
            currency = s.usage.currency;
        }
    }
    lv_obj_t *sum = card(104);
    key(sum, "RUNNING", 22, 16, 100);
    big(sum, num(running), nullptr, 22, 34, 40, OK);
    key(sum, "QUEUED", 130, 16, 100);
    big(sum, num(queued), nullptr, 130, 34, 40, TEXT);
    key(sum, "BLOCKED", 238, 16, 100);
    big(sum, num(blocked + failed), nullptr, 238, 34, 40, blocked + failed > 0 ? WARN : TEXT);
    key(sum, "SPEND", 340, 16, 100);
    label(sum, cost > 0 ? fmt("%.2f %s", cost, currency).c_str() : DASH, 340, 44, 110, TEXT, 18, true);
    Y += 104 + GAP;
    for (int i = 0; i < o.nSystems; ++i) {
        const ops::System &s = o.systems[i];
        const double age = ops_age(s.sampled);
        const bool fresh = G.ops_net.connected && isfinite(age) && isfinite(s.ttl) && age <= s.ttl;
        if (s.board) {
            kordlet_board(s, fresh);
            continue;
        }
        lv_obj_t *c = card(64);
        dot(c, 20, 26, 10, fresh ? ops_color(s.status) : WARN);
        label(c, s.name, 40, 10, 240, TEXT, 16, true);
        label(c, fmt("%s \xC2\xB7 %s", s.provider, fresh ? s.status : "stale").c_str(), 40, 34, 240, TEXT3, 13);
        label(c, fmt("%s run \xC2\xB7 %s queued", num(s.running).c_str(), num(s.queued).c_str()).c_str(), 250, 22, 190,
              TEXT2, 14, true, LV_TEXT_ALIGN_RIGHT);
        Y += 64 + 10;
    }
    // With Kordlet, tasks are its cards and agents its sessions and machines.
    const bool cards = ops::kordletCards(o);
    static const char *const tabs[] = {"Agents", "Tasks", "Events"}, *const kordlet_tabs[] = {"Sessions", "Cards", "Events"};
    lv_obj_t *t = capsule(C, 0, Y, CW, 46, cards ? kordlet_tabs : tabs, 3, G.ops_tab, ACC, 15);
    lv_obj_add_event_cb(t, ops_tab_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    Y += 46 + GAP;
    if (G.ops_tab == 0) {
        // Kordlet sessions before machines (and anything else after both).
        int order[ops::MAX_AGENTS], n = 0;
        for (int pass = 0; pass < 3; ++pass)
            for (int i = 0; i < o.nAgents; ++i) {
                const bool session = !strcmp(o.agents[i].kind, "session"), machine = !strcmp(o.agents[i].kind, "machine");
                if ((pass == 0 && session) || (pass == 1 && machine) || (pass == 2 && !session && !machine)) {
                    order[n++] = i;
                }
            }
        for (int k = 0; k < n; ++k) {
            const int i = order[k];
            const ops::Agent &a = o.agents[i];
            const double age = ops_age(a.heartbeat);
            const bool fresh = G.ops_net.connected && isfinite(age) && isfinite(a.ttl) && age <= a.ttl;
            lv_obj_t *c = card(84);
            dot(c, 20, 20, 10, fresh ? ops_color(a.state) : WARN);
            label(c, a.name, 40, 11, 250, TEXT, 16, true);
            label(c, fresh ? (a.stage[0] ? a.stage : a.state) : "stale", 290, 13, 150, fresh ? ops_color(a.state) : WARN, 13,
                  true, LV_TEXT_ALIGN_RIGHT);
            const bool session = !strcmp(a.kind, "session"), machine = !strcmp(a.kind, "machine");
            label(c, fmt("%s \xC2\xB7 %s \xC2\xB7 %s", system_name(a.system), session ? "session" : machine ? "machine" : a.provider,
                         a.host[0] ? a.host : a.model).c_str(), 40, 36, 400, TEXT3, 13);
            // What it works on: the card's title when it is in this snapshot.
            const char *work = a.task;
            for (int j = 0; j < o.nTasks; ++j)
                if (o.tasks[j].system == a.system && !strcmp(o.tasks[j].id, a.task)) {
                    work = o.tasks[j].title;
                }
            label(c, a.activity[0] ? a.activity : work[0] ? work : ("heartbeat " + ago(a.heartbeat)).c_str(), 40, 56,
                  a.chat ? 360 : 400, a.activity[0] ? ACC.glow : TEXT2, 14);
            if (a.chat) {  // its conversation, read only
                icon(c, "chev", 420, 52, 18, TEXT3, 1.8f);
                lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
                press_fx(c);
                lv_obj_add_event_cb(c, session_tap_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(i)));
            }
            Y += 84 + 10;
        }
        if (!o.nAgents) {
            empty(cards ? "No sessions reported." : "No agents reported.");
        }
    } else if (G.ops_tab == 1) {
        for (int i = 0; i < o.nTasks; ++i) {
            const ops::Task &tk = o.tasks[i];
            if (!strcmp(tk.kind, "card")) {  // a Kordlet card: lane, status, what it needs from you
                const bool needs = tk.attention[0];
                lv_obj_t *c = card(needs ? 104 : 84);
                dot(c, 20, 20, 10, needs ? attention_color(tk.attentionLevel) : lane_color(tk.lane));
                label(c, tk.title, 40, 11, 290, TEXT, 16, true);
                label(c, tk.status[0] ? tk.status : tk.state, 330, 13, 110, ops_color(tk.state), 13, true, LV_TEXT_ALIGN_RIGHT);
                label(c, lane_label(tk.lane), 40, 36, 120, lane_color(tk.lane), 13, true);
                label(c, fmt("%s \xC2\xB7 %s", tk.project[0] ? tk.project : system_name(tk.system), ago(tk.updated).c_str()).c_str(),
                      40 + text_width(lane_label(tk.lane), 13) + 12, 36, 300, TEXT3, 13);
                if (needs)
                    label(c, fmt("Needs you: %s", tk.attention).c_str(), 40, 60, tk.chat ? 360 : 400,
                          attention_color(tk.attentionLevel), 14, true);
                if (tk.chat) {
                    icon(c, "chev", 420, needs ? 62 : 52, 18, TEXT3, 1.8f);
                    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
                    press_fx(c);
                    lv_obj_add_event_cb(c, card_tap_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(i)));
                }
                Y += (needs ? 104 : 84) + 10;
                continue;
            }
            lv_obj_t *c = card(84);
            dot(c, 20, 20, 10, ops_color(tk.state));
            label(c, tk.title, 40, 11, 300, TEXT, 16, true);
            label(c, tk.state, 340, 13, 100, ops_color(tk.state), 13, true, LV_TEXT_ALIGN_RIGHT);
            label(c, fmt("%s \xC2\xB7 %s \xC2\xB7 %s", system_name(tk.system), tk.agent[0] ? tk.agent : "unassigned",
                         ago(tk.updated).c_str()).c_str(), 40, 36, 400, TEXT3, 13);
            if (isfinite(tk.progress)) {
                bar(c, 40, 62, 400, 6, int(tk.progress), ACC, false);
            }
            Y += 84 + 10;
        }
        if (!o.nTasks) {
            empty(cards ? "No cards reported." : "No tasks reported.");
        }
    } else {
        for (int i = 0; i < o.nEvents; ++i) {
            const ops::Event &ev = o.events[i];
            const bool bad = !strcmp(ev.severity, "error") || !strcmp(ev.severity, "critical");
            const bool warn = !strcmp(ev.severity, "warning");
            lv_obj_t *c = card(70);
            dot(c, 20, 20, 10, bad ? RED : warn ? WARN : ACC.glow);
            label(c, ev.title, 40, 11, 400, TEXT, 16, true);
            label(c, fmt("%s \xC2\xB7 %s \xC2\xB7 %s", system_name(ev.system), ev.kind, ago(ev.at).c_str()).c_str(), 40,
                  38, 400, TEXT3, 13);
            Y += 70 + 10;
        }
        if (!o.nEvents) {
            empty("No recent events.");
        }
    }
}

void poll_operations()
{
    if (G.demo) {
        if (!G.ops->valid || !G.ops->demo) {
            demo_operations(*G.ops);
        }
        G.ops_net.connected = true;
    } else {
        readOperations(*G.ops, G.ops_net);
    }
    if (G.ops->sequence != G.ops_sequence || G.ops_net.revision != G.ops_revision) {
        G.ops_sequence = G.ops->sequence;
        G.ops_revision = G.ops_net.revision;
        if (G.page == AGENTS) {
            G.dirty = true;
        }
    }
}

// ---- Kordlet chat (read only; replies happen in Kordlet) -----------------------------------------
void demo_chat(agentchat::Model &m)
{
    new (&m) agentchat::Model{};
    m.valid = true;
    m.card = G.chat_card;
    strlcpy(m.system, G.chat_system, sizeof(m.system));
    strlcpy(m.id, G.chat_id, sizeof(m.id));
    strlcpy(m.title, G.chat_title, sizeof(m.title));
    const char *roles[] = {"user", "assistant", "user", "assistant"};
    const char *texts[] = {"Make the board's Glimdock show the Kordlet lanes.",
                           "Done: Agents now shows Needs you and the six lanes, and cards open their chat.",
                           "Keep the chat read only.", "It is: replies stay in Kordlet."
                          };
    const double now = double(time(nullptr));
    for (int i = 0; i < 4; ++i) {
        auto &x = m.messages[m.count++];
        strlcpy(x.role, roles[i], sizeof(x.role));
        strlcpy(x.text, texts[i], sizeof(x.text));
        x.at = now - 600 + i * 120;
    }
    m.total = 4;
}

void render_chat()
{
    if (!G.chat_body) {
        return;
    }
    lv_obj_clean(G.chat_body);
    const agentchat::Model &m = *G.chat;
    int y = 0;
    if (!m.valid) {
        lv_obj_t *l = label(G.chat_body, G.chat_state.loading ? "Loading the conversation\xE2\x80\xA6" : G.chat_state.message,
                            10, 120, 400, G.chat_state.loading ? TEXT2 : WARN, 16, true, LV_TEXT_ALIGN_CENTER);
        wrap_label(l);
        if (!G.chat_state.loading) {
            lv_obj_t *b = field_button(G.chat_body, 130, 200, 160, 44, "Try again", 15, TEXT);
            on(b, [](lv_event_t *) {
                if (!G.demo) {
                    requestAgentChat(G.chat_system, G.chat_card, G.chat_id);
                }
            });
        }
        return;
    }
    if (m.total > m.count)
        y = 4 + lv_obj_get_height(label(G.chat_body, fmt("The last %u of %u messages", unsigned(m.count), unsigned(m.total)).c_str(),
                                        0, 0, 420, TEXT3, 13, false, LV_TEXT_ALIGN_CENTER)) + 8;
    for (int i = 0; i < m.count; ++i) {
        const agentchat::Message &msg = m.messages[i];
        const bool me = !strcmp(msg.role, "user"), sys = !strcmp(msg.role, "system");
        constexpr int W_ = 330;
        lv_obj_t *b = box(G.chat_body, sys ? 45 : me ? 420 - W_ : 0, y, W_, 40, 18);
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, lv_color_hex(me ? 0x1F4FD8 : sys ? 0x2A2210 : CARD), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(b, 12, 0);
        lv_obj_t *l = label(b, msg.text, 0, 0, W_ - 24, TEXT, 15);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_height(l, LV_SIZE_CONTENT);
        if (isfinite(msg.at)) {
            tm local{};
            const time_t t = time_t(msg.at);
            localtime_r(&t, &local);
            char when[16];
            strftime(when, sizeof(when), "%H:%M", &local);
            lv_obj_update_layout(l);
            label(b, (string(me ? "You" : sys ? "System" : "Agent") + " \xC2\xB7 " + when).c_str(), 0, lv_obj_get_height(l) + 4,
                  W_ - 24, me ? 0xC8D6FF : TEXT3, 12);
        }
        lv_obj_update_layout(b);
        y += lv_obj_get_height(b) + 10;
    }
    label(G.chat_body, "Read only: reply in Kordlet.", 0, y + 4, 420, TEXT3, 13, false, LV_TEXT_ALIGN_CENTER);
    lv_obj_update_layout(G.chat_body);
    lv_obj_scroll_to_y(G.chat_body, LV_COORD_MAX, LV_ANIM_OFF);
}

void close_chat()
{
    if (!G.chat_overlay) {
        return;
    }
    if (!G.demo) {
        stopAgentChat();
    }
    lv_obj_delete_async(G.chat_overlay);
    G.chat_overlay = G.chat_body = nullptr;
    G.chat_revision = ~0u;
}

void open_chat(const char *system, const char *id, bool card, const char *title)
{
    if (!G.chat || !agentchat::validId(system) || !agentchat::validId(id)) {
        return;
    }
    close_chat();
    strlcpy(G.chat_system, system, sizeof(G.chat_system));
    strlcpy(G.chat_id, id, sizeof(G.chat_id));
    strlcpy(G.chat_title, title, sizeof(G.chat_title));
    G.chat_card = card;
    new (G.chat) agentchat::Model{};
    G.chat_state = agentchat::State{};
    G.chat_state.loading = !G.demo;  // "Loading", never the idle hint, until the network answers
    if (G.demo) {
        demo_chat(*G.chat);
    }
    G.chat_requested = G.demo || requestAgentChat(system, card, id);
    lv_obj_t *o = box(G.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    G.chat_overlay = o;
    lv_obj_t *back = box(o, 56, 14, 44, 44, 22);
    lv_obj_set_style_bg_color(back, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    icon(back, "back", 10, 10, 24, TEXT, 2.0f);
    on(back, [](lv_event_t *) {
        close_chat();
    });
    press_fx(back);
    lv_obj_t *t = label(o, title, 112, 12, 300, TEXT, 18, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t, 24);
    label(o, card ? "Card chat \xC2\xB7 read only" : "Session chat \xC2\xB7 read only", 112, 38, 300, TEXT3, 13);
    G.chat_body = box(o, 30, 72, 420, 384);
    lv_obj_add_flag(G.chat_body, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM));
    lv_obj_set_scroll_dir(G.chat_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(G.chat_body, LV_SCROLLBAR_MODE_OFF);
    render_chat();
}

void poll_chat()
{
    if (!G.chat_overlay || G.demo) {
        return;
    }
    if (!G.chat_requested) {
        G.chat_requested = requestAgentChat(G.chat_system, G.chat_card, G.chat_id);
        return;
    }
    agentchat::State state;
    if (!readAgentChat(*G.chat, state)) {
        return;
    }
    if (!state.active) {
        return;    // not this request's state yet
    }
    G.chat_state = state;
    if (G.chat_state.revision != G.chat_revision) {
        G.chat_revision = G.chat_state.revision;
        render_chat();
    }
}

// ---- alert pop-ups ----------------------------------------------------------------------------
uint32_t hash(const char *a, const char *b = "")
{
    uint32_t h = 2166136261u;
    for (const char *s : {
                a, "\x1f", b
            })
        for (; *s; ++s) {
            h = (h ^ uint8_t(*s)) * 16777619u;
        }
    return h;
}

void dismiss_popup()
{
    if (G.popup) {
        lv_obj_delete_async(G.popup);
    }
    G.popup = nullptr;
}

void popup_y(void *o, int32_t v)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(o), v);
}

void popup_cb(lv_event_t *)
{
    const string node = G.popup_node;
    dismiss_popup();
    if (!node.empty()) {
        choose_node(node.c_str());
        open_page(HEALTH);
    }
}

void show_popup(const char *title, const char *detail, uint32_t color, const char *node_id)
{
    dismiss_popup();
    lv_obj_t *p = box(G.screen, 34, -100, 412, 86, 26);
    lv_obj_set_style_bg_color(p, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(color), 0);
    dot(p, 22, 22, 12, color);
    label(p, title, 46, 13, 350, TEXT, 17, true);
    label(p, detail, 22, 46, 370, TEXT2, 14);
    on(p, popup_cb);
    strlcpy(G.popup_node, node_id ? node_id : "", sizeof(G.popup_node));
    G.popup = p;
    G.popup_at = millis();
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p);
    lv_anim_set_exec_cb(&a, popup_y);
    lv_anim_set_values(&a, -100, 14);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
    if (G.sound) {
        boardAudioPlay(AudioCue::Alert);
    }
}

// New alerts on the shown system, and systems turning worse in the fleet.
void watch_alerts()
{
    const Snapshot &s = *G.snap;
    if (!s.valid) {
        return;
    }
    if (strcmp(G.seen_node, s.node.id)) {  // another system: learn its alerts quietly
        strlcpy(G.seen_node, s.node.id, sizeof(G.seen_node));
        G.seen_count = 0;
        G.seen_primed = false;
    }
    for (int i = 0; i < s.nAlerts; ++i) {
        const uint32_t h = hash(s.alerts[i].id, s.alerts[i].message);
        bool known = false;
        for (int j = 0; j < G.seen_count; ++j) {
            known |= G.seen[j] == h;
        }
        if (known) {
            continue;
        }
        if (G.seen_count == 32) {
            memmove(G.seen, G.seen + 1, sizeof(uint32_t) * 31);
            --G.seen_count;
        }
        G.seen[G.seen_count++] = h;
        if (G.seen_primed && G.active) {
            const bool critical = !strcmp(s.alerts[i].severity, "critical") || !strcmp(s.alerts[i].severity, "error");
            show_popup(fmt("%s \xC2\xB7 %s", s.node.name[0] ? s.node.name : s.host, s.alerts[i].severity).c_str(),
                       s.alerts[i].message, critical ? RED : WARN, s.node.id[0] ? s.node.id : nullptr);
        }
    }
    G.seen_primed = true;
    const bool primed = G.node_state_count >= 0;
    int count = 0;
    for (int i = 0; i < s.nNodes && i < int(MAX_NODES); ++i) {
        const uint32_t id = hash(s.nodes[i].id);
        const uint8_t st = node_state(s.nodes[i]);
        for (int j = 0; primed && j < G.node_state_count; ++j)
            if (G.node_states[j].id == id && st > G.node_states[j].state && G.active)
                show_popup(fmt("%s %s", s.nodes[i].name, st == 2 ? "is offline" : "needs attention").c_str(),
                           s.nodes[i].summary.status[0] ? s.nodes[i].summary.status : s.nodes[i].status, state_color(st),
                           s.nodes[i].id);
        G.node_states[count++] = {id, st};
    }
    G.node_state_count = count;
}

// ---- private conversations -----------------------------------------------------------------
string when(double epoch)
{
    const double now = double(time(nullptr));
    if (!isfinite(epoch) || now < 1700000000) {
        return DASH;
    }
    return span(std::max(0.0, now - epoch)) + " ago";
}

// ---- chrome -------------------------------------------------------------------------------
void tab_cb(lv_event_t *e)
{
    const int i = capsule_selected(lv_event_get_current_target_obj(e));
    if (G.tabs && i >= 0 && i < G.tabs->count) {
        open_page(G.tabs->pages[i]);
    }
}

const TabSet *tabs_for_view()
{
    static const TabSet fleet{FLEET_PAGES, FLEET_LABELS, 4}, host{HOST_PAGES, HOST_LABELS, 5},
           server{SERVER_PAGES, SERVER_LABELS, 4}, printer{PRINTER_PAGES, PRINTER_LABELS, 3};
    if (G.fleet) {
        return &fleet;
    }
    const NodeDescriptor &n = *chosen();
    const bool here = !loading_node();
    if (is_printer(n) || (here && G.snap->printer.present)) {
        return &printer;
    }
    if (is_host(n) || (here && G.snap->nGuests)) {
        return &host;
    }
    return &server;
}

void update_tabs()
{
    const TabSet *t = tabs_for_view();
    int index = 0;
    const Page shown_page = G.page == NODES || G.page == NODE_FORM ? SETTINGS : G.page;
    for (int i = 0; i < t->count; ++i)
        if (t->pages[i] == shown_page) {
            index = i;
        }
    if (t != G.tabs || !G.tab_bar) {
        if (G.tab_bar) {
            lv_obj_delete(G.tab_bar);
        }
        G.tabs = t;
        G.tab_bar = capsule(G.screen, 34, 412, 412, 50, t->labels, t->count, index, ACC, t->count > 4 ? 13 : 15);
        lv_obj_add_event_cb(G.tab_bar, tab_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    } else if (capsule_selected(G.tab_bar) != index) {
        capsule_select(G.tab_bar, index, true);
    }
}

void picker_cb(lv_event_t *)
{
    close_overlay();
    lv_obj_t *o = box(G.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, [](lv_event_t *) {
        close_overlay();
    }, LV_EVENT_CLICKED, nullptr);
    G.overlay = o;
    label(o, "Choose a system", 40, 24, 400, TEXT, 26, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *list = box(o, 0, 70, PANEL, 340);
    lv_obj_add_flag(list, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    const Snapshot &s = *G.snap;
    for (int i = -1; i < s.nNodes; ++i) {
        lv_obj_t *r = glass(list, 10, 4 + 64 * (i + 1), 460, 56, 28, ACC);
        press_fx(r);
        on(r, node_cb, reinterpret_cast<void *>(intptr_t(i)));
        if (i < 0) {
            icon(r, "rooms", 18, 16, 24, TEXT, 1.6f);
            label(r, "All systems", 54, 16, 300, TEXT, 18, true);
            continue;
        }
        const NodeDescriptor &n = s.nodes[i];
        dot(r, 24, 23, 10, state_color(node_state(n)));
        label(r, n.name[0] ? n.name : n.id, 54, 8, 300, TEXT, 17, true);
        label(r, nodeTypeName(n), 54, 31, 300, TEXT3, 13);
        if (!G.fleet && !strcmp(n.id, s.node.id)) {
            icon(r, "check", 418, 16, 24, ACC.glow, 2.0f);
        }
    }
    lv_obj_t *close = field_button(o, 150, 420, 180, 44, "Close", 16, TEXT2);
    on(close, [](lv_event_t *) {
        close_overlay();
    });
    stagger(list, 0, 15);
}

void close_overlay()
{
    if (G.overlay) {
        lv_obj_delete_async(G.overlay);
    }
    G.overlay = nullptr;
    G.editor_area = nullptr;
}

void build_chrome()
{
    lv_obj_t *s = G.screen;
    lv_obj_t *mark = box(s, 56, 12, 28, 28, 9);
    style_fill(mark, VIOLET, false);
    label(mark, "g", 0, 0, 28, 0xFFFFFF, 20, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *word = label(s, "glimdock", 92, 12, 0, TEXT, 20, true);
    label(s, ".", 92 + text_width("glimdock", 20) - 1, 12, 0, OK, 20, true);
    (void)word;
    G.clock = label(s, "--:--", 300, 13, 96, TEXT2, 18, true, LV_TEXT_ALIGN_RIGHT);
    lv_obj_t *gear = box(s, 404, 6, 40, 40, 20);
    icon(gear, "sliders", 8, 8, 24, TEXT2, 1.8f);
    on(gear, goto_settings);
    lv_obj_t *pick = box(s, 10, 50, 318, 44, 22);
    lv_obj_set_style_bg_color(pick, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(pick, LV_OPA_COVER, 0);
    press_fx(pick);
    on(pick, picker_cb);
    G.picker = label(pick, "All systems", 20, 11, 250, TEXT, 18, true);
    icon(pick, "down", 282, 12, 20, ACC.glow, 2.0f);
    lv_obj_t *chip = box(s, 338, 50, 132, 44, 22);
    lv_obj_set_style_bg_color(chip, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(EDGE), 0);
    G.status_dot = dot(chip, 20, 17, 10, OK);
    G.status = label(chip, "", 38, 12, 86, TEXT, 15, true);
    G.content = box(s, 10, CONTENT_Y, CW, CONTENT_H);
    // Clickable too: LVGL only finds scrollables from a touched object
    // upwards, and box() leaves objects unclickable, so drags that started on
    // a plain card never scrolled the page.
    lv_obj_add_flag(G.content,
                    lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_scroll_dir(G.content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(G.content, LV_SCROLLBAR_MODE_OFF);
}

void update_header()
{
    const Feed f = feed();
    if (strcmp(lv_label_get_text(G.status), f.text)) {
        lv_label_set_text(G.status, f.text);
        lv_obj_set_style_text_color(G.status, lv_color_hex(f.color), 0);
        lv_obj_set_style_bg_color(G.status_dot, lv_color_hex(f.color), 0);
    }
    const NodeDescriptor *n = chosen();
    const char *name = !n ? "All systems" : n->name[0] ? n->name : G.snap->host[0] ? G.snap->host : "This system";
    if (strcmp(lv_label_get_text(G.picker), name)) {
        lv_label_set_text(G.picker, name);
    }
    tm local{};
    uint8_t zone = 0;
    char clock[8] = "--:--";
    Preferences p;
    if (p.begin("display", true)) {
        zone = p.getUChar("clockZone", 0) % DISPLAY_CLOCK_ZONE_COUNT;
        p.end();
    }
    if (displayClockTm(time(nullptr), zone, local)) {
        snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
    }
    if (strcmp(lv_label_get_text(G.clock), clock)) {
        lv_label_set_text(G.clock, clock);
    }
}

bool finger_down()
{
    for (lv_indev_t *i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i))
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER &&
                (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED || lv_indev_get_scroll_obj(i))) {
            return true;
        }
    return lv_obj_is_scrolling(G.content);
}

Page rendered_page = SETTINGS;
bool rendered_fleet = true, has_rendered = false;

void rebuild()
{
    G.dirty = false;
    const bool same = has_rendered && rendered_page == G.page && rendered_fleet == G.fleet;
    const int32_t scroll = same ? lv_obj_get_scroll_y(G.content) : 0;
    lv_obj_clean(G.content);
    C = G.content;
    Y = 0;
    set_entry_animations(!same);  // fresh data on the same page: no grow-ins
    switch (G.page) {
    case SYSTEMS: page_systems(); break;
    case CLOCK: page_clock(); break;
    case SETTINGS: page_settings(); break;
    case OVERVIEW: page_overview(); break;
    case GUESTS: page_guests(); break;
    case STORAGE: page_storage(); break;
    case SENSORS: page_sensors(); break;
    case HEALTH: page_health(); break;
    case JOB: page_job(); break;
    case TEMPS: page_temps(); break;
    case AGENTS: page_agents(); break;
    case NODES: page_nodes(); break;
    case NODE_FORM: page_node_form(); break;
    }
    if (redirect) {  // the page chose another view (one system: its overview)
        redirect = false;
        has_rendered = false;
        set_entry_animations(true);
        rebuild();
        return;
    }
    set_entry_animations(true);
    box(G.content, 0, Y, 1, 8);  // bottom breathing room
    update_tabs();
    update_header();
    if (scroll) {
        lv_obj_update_layout(G.content);
        lv_obj_scroll_to_y(G.content, scroll, LV_ANIM_OFF);
    }
    if (!same) {
        stagger(G.content, 0, 15);
    }
    rendered_page = G.page;
    rendered_fleet = G.fleet;
    has_rendered = true;
}

// Pages change from inside tap handlers on objects the rebuild deletes, so
// the rebuild happens on the next poll.
void open_page(Page p)
{
    if (p != AGENTS) {
        close_chat();
    }
    if (p == AGENTS && G.page != AGENTS) {
        requestOperationsRefresh();
        poll_operations();
    }
    G.page = p;
    G.dirty = true;
}

// ---- data ------------------------------------------------------------------------------------
void poll(lv_timer_t *)
{
#ifndef HOMELAB_NATIVE_PREVIEW
    tickRuntimeHealth();
#endif
    const uint32_t now = millis();
    if (now - G.last_poll >= 500) {
        G.last_poll = now;
        if (G.demo) {
            makeDemo(*G.snap);
        } else {
            readSnapshot(*G.snap, G.net);
            readConnectionSettings(G.conn);
        }
        if (G.snap->sequence != G.sequence || G.net.revision != G.revision || G.conn.revision != G.conn_revision) {
            if (G.snap->sequence != G.sequence) {
                sample_history();
            }
            G.sequence = G.snap->sequence;
            G.revision = G.net.revision;
            G.conn_revision = G.conn.revision;
            G.dirty = true;
        }
        watch_alerts();
        if (G.page == NODES || G.page == NODE_FORM || G.node_save_pending || G.node_delete_pending) {
            poll_nodes();
        }
        if (G.page == AGENTS) {
            poll_operations();
        }
        poll_chat();
    }
    if (!G.active) {
        return;
    }
    if (now - G.last_clock >= 1000) {
        G.last_clock = now;
        update_header();
        if (G.page == CLOCK) {
            G.dirty = true;
        }
    }
    if (G.popup && now - G.popup_at > 6000) {
        dismiss_popup();
    }
    if (G.dirty && !G.overlay && !finger_down()) {
        rebuild();
    }
}
}  // namespace

void glim_ui_start(lv_obj_t *screen)
{
    if (G.started) {
        return;
    }
    G.started = true;
    G.screen = screen;
#ifdef ESP_PLATFORM
    G.snap = static_cast<Snapshot *>(heap_caps_malloc(sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    G.snap = static_cast<Snapshot *>(malloc(sizeof(Snapshot)));
#endif
    if (!G.snap) {
        label(screen, "Glimdock needs PSRAM", 40, 220, 400, RED, 20, true, LV_TEXT_ALIGN_CENTER);
        return;
    }
    new (G.snap) Snapshot{};
#ifdef ESP_PLATFORM
    G.cfg = static_cast<NodeConfiguration *>(heap_caps_malloc(sizeof(NodeConfiguration), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    G.ops = static_cast<ops::Snapshot *>(heap_caps_malloc(sizeof(ops::Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    G.cfg = static_cast<NodeConfiguration *>(malloc(sizeof(NodeConfiguration)));
    G.ops = static_cast<ops::Snapshot *>(malloc(sizeof(ops::Snapshot)));
#endif
    if (G.cfg) new (G.cfg) NodeConfiguration {};
    if (G.ops) new (G.ops) ops::Snapshot{};
#ifdef ESP_PLATFORM
    G.chat = static_cast<agentchat::Model *>(heap_caps_malloc(sizeof(agentchat::Model), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    G.chat = static_cast<agentchat::Model *>(malloc(sizeof(agentchat::Model)));
#endif
    if (G.chat) new (G.chat) agentchat::Model {};
    if (!G.cfg || !G.ops) {
        label(screen, "Glimdock needs PSRAM", 40, 220, 400, RED, 20, true, LV_TEXT_ALIGN_CENTER);
        return;
    }
    unsigned volume = DEFAULT_SOUND_VOLUME;
    Preferences p;
    if (p.begin("display", true)) {
        G.sound = p.getBool("sound", true);
        volume = clampSoundVolume(p.getUChar("volume", DEFAULT_SOUND_VOLUME));
        p.end();
    }
    boardAudioVolume(volume);
    boardAudioInit(G.sound);
#ifndef HOMELAB_NATIVE_PREVIEW
    startRuntimeHealth();
#endif
    startNetwork();
#ifndef HOMELAB_NATIVE_PREVIEW
    glim_iris_serial_register();
#endif
    build_chrome();
    rebuild();
    lv_timer_create(poll, 50, nullptr);
}

void glim_ui_set_active(bool active)
{
    G.active = active && G.started;
    if (G.active) {
        G.dirty = true;
    }
}

// Host preview: a page with demo data (node -1: all systems) or the setup
// state, and the overlays (0 system picker, 1 connection form, 2 editor).
void glim_ui_preview(int page, int node, bool demo)
{
    close_overlay();
    dismiss_popup();
    G.demo = demo;
    G.active = true;
    if (demo) {
        makeDemo(*G.snap);
        if (node >= 0 && node < G.snap->nNodes) {
            G.snap->node = G.snap->nodes[node];
            makeDemo(*G.snap);
        }
    } else {
        new (G.snap) Snapshot{};
        G.net = NetworkState{};
    }
    G.fleet = node < 0;
    G.node_key[0] = 0;
    if (!G.fleet) {
        strlcpy(G.node_key, G.snap->node.id, sizeof(G.node_key));
    }
    G.history_count = 0;
    for (int i = 0; i < 24; ++i) {
        G.history[i] = 18 + 9 * sinf(i * 0.55f) + (i % 3) * 2;
        ++G.history_count;
    }
    strlcpy(G.history_node, G.snap->node.id, sizeof(G.history_node));
    G.page = Page(page);
    has_rendered = false;
    rebuild();
}

void glim_ui_preview_overlay(int which)
{
    if (which == 0) {
        picker_cb(nullptr);
    } else if (which == 1) {
        open_form();
    } else if (which == 2) {
        load_draft();
        open_editor(&G.draft[2], FIELD_TITLES[2], 240, false, nullptr, open_form);
    } else if (which == 3) {  // the first collector node, in its form
        readNodeConfiguration(*G.cfg);
        if (G.cfg->count) {
            begin_node(G.cfg->nodes[0], true);
        }
        rebuild();
    } else if (which == 4) {
        show_popup("Virtual host \xC2\xB7 critical", "Disk sda reports 3 reallocated sectors", RED, nullptr);
    } else if (which == 5 || which == 6) {  // the Kordlet cards tab, then the first card's chat
        poll_operations();
        G.ops_tab = 1;
        rebuild();
        if (which == 6 && G.ops->nTasks) open_chat(G.ops->systems[G.ops->tasks[0].system].id, G.ops->tasks[0].id, true,
                                                       G.ops->tasks[0].title);
    }
}
