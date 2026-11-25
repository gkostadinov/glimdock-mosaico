// SPDX-License-Identifier: Apache-2.0
// The launcher's Glimdock page: a fleet summary read from the snapshot the
// network worker publishes, without touching Glimdock's UI.
#include <math.h>
#include <string.h>

#include <stdarg.h>
#include <stdio.h>

#include <new>
#include <string>

#ifndef HOMELAB_NATIVE_PREVIEW
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include "glimdock_app.h"
#include "model.h"
#include "operations_model.h"

namespace {
bool started_flag = false;
float history[32];
int history_count = 0;
uint32_t last_sample = 0;

uint8_t node_state(const NodeDescriptor &n)
{
    if (!strcmp(n.status, "offline") || !strcmp(n.summary.status, "offline")) return 2;
    const char *s = n.summary.status[0] ? n.summary.status : n.status;
    return !strcmp(s, "healthy") ? 0 : 1;
}

float average(const float *values, int count)
{
    float sum = 0;
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (isfinite(values[i])) {
            sum += values[i];
            ++n;
        }
    return n ? sum / n : NAN;
}

void fill(glimdock_summary_t *out, const Snapshot &s, const NetworkState &net)
{
    out->ready = s.valid;
    out->fresh = s.valid && (net.wifi || s.demo);
    out->alerts = s.nAlerts;
    out->nodes = s.nNodes;
    if (s.nNodes) {
        float cpu[MAX_NODES], mem[MAX_NODES];
        for (int i = 0; i < s.nNodes; ++i) {
            const NodeDescriptor &n = s.nodes[i];
            const uint8_t state = node_state(n);
            if (state == 0) ++out->healthy;
            cpu[i] = n.summary.available ? n.summary.cpu : NAN;
            mem[i] = n.summary.available ? n.summary.memory : NAN;
            if (out->item_count < 3) {
                auto &item = out->items[out->item_count++];
                strlcpy(item.name, n.name[0] ? n.name : n.id, sizeof(item.name));
                item.state = state;
                if (isfinite(n.summary.progress) && n.summary.printState[0] && !strcmp(n.summary.printState, "printing"))
                    snprintf(item.note, sizeof(item.note), "%d%%", int(lroundf(n.summary.progress)));
                else if (isfinite(n.summary.cpu))
                    snprintf(item.note, sizeof(item.note), "%d%%", int(lroundf(n.summary.cpu)));
            }
        }
        out->cpu = average(cpu, s.nNodes);
        out->memory = average(mem, s.nNodes);
    } else {
        out->healthy = s.valid ? 1 : 0;
        out->cpu = s.cpu;
        out->memory = isfinite(s.memUsed) && s.memTotal > 0 ? float(100.0 * s.memUsed / s.memTotal) : NAN;
        if (s.host[0]) {
            auto &item = out->items[out->item_count++];
            strlcpy(item.name, s.host, sizeof(item.name));
            item.state = out->fresh ? 0 : 1;
        }
    }
    if (!s.valid)
        strlcpy(out->status, net.message[0] ? net.message : "Connecting to the collector\xE2\x80\xA6", sizeof(out->status));
    else if (s.nNodes)
        snprintf(out->status, sizeof(out->status), "%d of %d systems healthy%s", out->healthy, out->nodes,
                 s.demo ? " \xC2\xB7 demo" : out->fresh ? " \xC2\xB7 live" : "");
    else
        snprintf(out->status, sizeof(out->status), "%s%s", s.host[0] ? s.host : "One system",
                 s.demo ? " \xC2\xB7 demo" : out->fresh ? " \xC2\xB7 live" : "");
}
}  // namespace

void glimdock_summary_mark_started() { started_flag = true; }

extern "C" void glimdock_app_summary(glimdock_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cpu = out->memory = NAN;
    out->started = started_flag;
    // Only the fields the summary needs, read under the publisher's lock.
    static Snapshot *copy = nullptr;
    static NetworkState net;
    if (!copy) copy = new (std::nothrow) Snapshot();
    bool have = false;
    if (copy && publishedSnapshot) {
#ifdef HOMELAB_NATIVE_PREVIEW
        have = readSnapshot(*copy, net);
#else
        if (dataMutex && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (publishedSnapshot) {
                memcpy(static_cast<void *>(copy), publishedSnapshot, offsetof(Snapshot, guests));
                net = networkState;
                have = true;
            }
            xSemaphoreGive(dataMutex);
        }
#endif
    }
    if (have) {
        fill(out, *copy, net);
    } else {
        strlcpy(out->status, started_flag ? "Starting\xE2\x80\xA6" : "Homelab monitor", sizeof(out->status));
    }
    const uint32_t now = millis();
    if (isfinite(out->cpu) && (history_count == 0 || now - last_sample >= 2000)) {
        last_sample = now;
        if (history_count == 32) memmove(history, history + 1, sizeof(float) * 31);
        else ++history_count;
        history[history_count - 1] = out->cpu;
    }
    memcpy(out->history, history, sizeof(history));
    out->history_count = history_count;
}

// ---- Swifty's view of the homelab ---------------------------------------------------------
namespace {
struct Text {
    std::string s;
    void add(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char line[256];
        va_list a;
        va_start(a, format);
        vsnprintf(line, sizeof(line), format, a);
        va_end(a);
        s += line;
    }
};

std::string pct(float v) { return isfinite(v) ? std::to_string(int(lroundf(v))) + "%" : "?"; }
std::string gib(double bytes) { char b[24]; snprintf(b, sizeof(b), "%.1f GiB", bytes / 1073741824.0); return isfinite(bytes) ? b : "?"; }

template <typename T> T *psram_object()
{
#ifdef HOMELAB_NATIVE_PREVIEW
    return new (std::nothrow) T();
#else
    void *p = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? new (p) T() : nullptr;
#endif
}

void pause_ms(int ms)
{
#ifndef HOMELAB_NATIVE_PREVIEW
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    (void)ms;
#endif
}

void fleet(Text &t, const Snapshot &s)
{
    if (!s.nNodes) {
        t.add("One system: %s, CPU %s, memory %s.\n", s.host[0] ? s.host : "?", pct(s.cpu).c_str(),
              isfinite(s.memUsed) && s.memTotal > 0 ? pct(float(100.0 * s.memUsed / s.memTotal)).c_str() : "?");
        return;
    }
    t.add("%d systems:\n", s.nNodes);
    for (int i = 0; i < s.nNodes; ++i) {
        const NodeDescriptor &n = s.nodes[i];
        const NodeSummary &m = n.summary;
        t.add("- %s (%s %s): %s", n.name[0] ? n.name : n.id, n.platform, n.type, m.status[0] ? m.status : n.status);
        if (m.available) {
            t.add(", CPU %s, memory %s", pct(m.cpu).c_str(), pct(m.memory).c_str());
            if (isfinite(m.temp)) t.add(", %d C", int(lroundf(m.temp)));
            if (m.guests >= 0) t.add(", %d guests", int(m.guests));
            if (m.printState[0]) t.add(", printer %s %s", m.printState, pct(m.progress).c_str());
        }
        t.add("\n");
    }
}

void system(Text &t, const Snapshot &s)
{
    t.add("Shown system %s%s: CPU %s", s.node.name[0] ? s.node.name : s.host, s.demo ? " (demo data)" : "", pct(s.cpu).c_str());
    if (isfinite(s.load[0])) t.add(", load %.2f %.2f %.2f", double(s.load[0]), double(s.load[1]), double(s.load[2]));
    if (isfinite(s.memUsed) && s.memTotal > 0) t.add(", memory %s of %s", gib(s.memUsed).c_str(), gib(s.memTotal).c_str());
    if (isfinite(s.temp)) t.add(", %d C", int(lroundf(s.temp)));
    if (isfinite(s.watts)) t.add(", %d W", int(lroundf(s.watts)));
    if (isfinite(s.uptime)) t.add(", up %d days", int(s.uptime / 86400));
    t.add(".\n");
    if (s.nGuests) {
        int running = 0;
        for (int i = 0; i < s.nGuests; ++i) running += !strcmp(s.guests[i].status, "running");
        t.add("Guests (%d running of %d):", running, s.nGuests);
        for (int i = 0; i < s.nGuests && i < 24; ++i)
            t.add(" %s %s%s%s;", s.guests[i].name, s.guests[i].status,
                  isfinite(s.guests[i].cpu) ? (" CPU " + pct(s.guests[i].cpu)).c_str() : "", "");
        t.add("\n");
    }
    for (int i = 0; i < s.nPools; ++i)
        t.add("Storage %s: %s used of %s (%s)\n", s.pools[i].name, gib(s.pools[i].used).c_str(), gib(s.pools[i].total).c_str(),
              s.pools[i].status);
    for (int i = 0; i < s.nDisks; ++i)
        if (s.disks[i].health[0] && strcmp(s.disks[i].health, "PASSED") && strcmp(s.disks[i].health, "OK") && strcmp(s.disks[i].health, "healthy"))
            t.add("Disk %s: %s\n", s.disks[i].name, s.disks[i].health);
    if (s.printer.present)
        t.add("Printer %s: %s %s%s\n", s.printer.name, s.printer.state, s.printer.filename,
              isfinite(s.printer.progress) ? (" " + pct(s.printer.progress)).c_str() : "");
}

void alerts(Text &t, const Snapshot &s)
{
    if (!s.nAlerts) {
        t.add("No alerts.\n");
        return;
    }
    t.add("%d alert(s):\n", s.nAlerts);
    for (int i = 0; i < s.nAlerts; ++i) t.add("- %s: %s\n", s.alerts[i].severity, s.alerts[i].message);
}

void agents(Text &t, const ops::Snapshot &o, const ops::Network &net)
{
    if (!o.valid) {
        t.add("Agent operations: %s\n", net.message);
        return;
    }
    for (int i = 0; i < o.nSystems; ++i) {
        const ops::System &y = o.systems[i];
        t.add("%s (%s): %s", y.name, y.provider, y.status);
        if (y.board && isfinite(y.needsYou)) t.add(", %d card(s) need you", int(y.needsYou));
        if (isfinite(y.running)) t.add(", %d running", int(y.running));
        if (isfinite(y.queued)) t.add(", %d queued", int(y.queued));
        t.add("\n");
    }
    int shown = 0;
    for (int i = 0; i < o.nAgents && shown < 12; ++i, ++shown) {
        const ops::Agent &a = o.agents[i];
        t.add("- %s %s: %s%s%s\n", !strcmp(a.kind, "session") ? "session" : "agent", a.name, a.stage[0] ? a.stage : a.state,
              a.activity[0] ? ", " : "", a.activity);
    }
    shown = 0;
    for (int i = 0; i < o.nTasks && shown < 16; ++i) {
        const ops::Task &k = o.tasks[i];
        if (k.lane == 5) continue;  // done
        ++shown;
        t.add("- card \"%s\" (%s%s%s)%s%s\n", k.title, k.lane >= 0 && k.lane < ops::LANES ? ops::LANE_LABELS[k.lane] : k.state,
              k.project[0] ? ", " : "", k.project, k.attention[0] ? " needs you: " : "", k.attention);
    }
}
}  // namespace

extern "C" void glimdock_app_report(const char *topic, char *out, size_t capacity)
{
    const std::string want = topic ? topic : "";
    static Snapshot *snap = nullptr;
    static ops::Snapshot *ops_snap = nullptr;
    if (!snap) snap = psram_object<Snapshot>();
    if (!ops_snap) ops_snap = psram_object<ops::Snapshot>();
    Text t;
    if (!started_flag) t.add("Glimdock has not started yet (open it once).\n");
    NetworkState net;
    bool have = false;
    for (int i = 0; snap && i < 50 && !(have = readSnapshot(*snap, net)); ++i) pause_ms(10);
    if (want != "agents") {
        if (!have || !snap->valid)
            t.add("No homelab data yet: %s\n", net.message[0] ? net.message : "connecting to the collector");
        else {
            if (!net.wifi && !snap->demo) t.add("(Last known data: the collector is not answering.)\n");
            if (want.empty() || want == "fleet") fleet(t, *snap);
            if (want.empty() || want == "system") system(t, *snap);
            alerts(t, *snap);
        }
    }
    if (want.empty() || want == "agents") {
        ops::Network on;
        bool got = false;
        for (int i = 0; ops_snap && i < 50 && !(got = readOperations(*ops_snap, on)); ++i) pause_ms(10);
        if (got) agents(t, *ops_snap, on);
    }
    strlcpy(out, t.s.c_str(), capacity);
}
