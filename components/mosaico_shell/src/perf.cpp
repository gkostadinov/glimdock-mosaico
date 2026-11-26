// SPDX-License-Identifier: Apache-2.0
// Frame profiler and a scripted scroll, for measuring how smoothly the panel
// scrolls (service 0x4744):
//   method 11: payload 1 resets; replies the frames, average render and
//              flush-wait times and frame rate since the reset.
//   method 12: scrolls the biggest scrollable thing on the active screen back
//              and forth for 3 s, as a finger would, frame by frame; the stats
//              are reset at its start and logged at its end.
#include "perf.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include "bsp/esp_mosaico.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "misc/lv_timer_private.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {
const char *TAG = "perf";
constexpr uint16_t RPC_SERVICE = 0x4744;
constexpr int64_t SCROLL_US = 3000000;

struct {
    std::atomic<uint32_t> frames{0}, slow{0};
    std::atomic<int64_t> render_us{0}, wait_us{0}, worst_us{0}, since{0};
    int64_t render_start = 0, wait_start = 0;
    // scripted scroll
    lv_obj_t *target = nullptr;
    bool vertical = true;
    int32_t range = 0, origin = 0;
    int64_t scroll_start = 0;
    lv_timer_t *timer = nullptr;
} P;

void reset()
{
    P.frames = 0, P.slow = 0, P.render_us = 0, P.wait_us = 0, P.worst_us = 0;
    P.since = esp_timer_get_time();
}

void on_display(lv_event_t *e)
{
    const int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START:
        P.render_start = now;
        break;
    case LV_EVENT_RENDER_READY: {
        if (!P.render_start) {
            break;
        }
        const int64_t t = now - P.render_start;
        P.render_us += t;
        if (t > P.worst_us) {
            P.worst_us = t;
        }
        if (t > 33000) {
            ++P.slow;
        }
        ++P.frames;
        P.render_start = 0;
        break;
    }
    case LV_EVENT_FLUSH_WAIT_START:
        P.wait_start = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (P.wait_start) {
            P.wait_us += now - P.wait_start;
        }
        P.wait_start = 0;
        break;
    default:
        break;
    }
}

int summary(char *out, size_t capacity)
{
    const uint32_t n = P.frames;
    const double secs = (esp_timer_get_time() - P.since) / 1e6;
    return snprintf(out, capacity,
                    "frames %u in %.1f s (%.1f fps) | render %.1f ms avg, %.1f worst, %u over 33 ms | flush wait %.1f ms avg",
                    unsigned(n), secs, secs > 0 ? n / secs : 0, n ? P.render_us / 1000.0 / n : 0, P.worst_us / 1000.0,
                    unsigned(P.slow), n ? P.wait_us / 1000.0 / n : 0);
}

esp_err_t stats_rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    if (request->payload_size >= 1 && request->payload[0] == 1) {
        reset();
    }
    const int n = summary(reinterpret_cast<char *>(out), capacity);
    *size = n < 0 ? 0 : std::min<size_t>(size_t(n), capacity);
    return ESP_OK;
}

// The scrollable object with the most to scroll, on the active screen.
void find(lv_obj_t *o, lv_obj_t **best, int32_t *best_range, bool *vertical)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_obj_update_layout(o);
        const int32_t v = lv_obj_get_scroll_top(o) + lv_obj_get_scroll_bottom(o);
        const int32_t h = lv_obj_get_scroll_left(o) + lv_obj_get_scroll_right(o);
        if (std::max(v, h) > *best_range) {
            *best = o;
            *best_range = std::max(v, h);
            *vertical = v >= h;
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        find(lv_obj_get_child(o, int32_t(i)), best, best_range, vertical);
    }
}

void scroll_step(lv_timer_t *t)
{
    const int64_t elapsed = esp_timer_get_time() - P.scroll_start;
    if (!P.target || !lv_obj_is_valid(P.target) || elapsed > SCROLL_US) {
        if (P.target && lv_obj_is_valid(P.target)) {
            if (P.vertical) {
                lv_obj_scroll_to_y(P.target, P.origin, LV_ANIM_OFF);
            } else {
                lv_obj_scroll_to_x(P.target, P.origin, LV_ANIM_OFF);
            }
        }
        lv_timer_delete(t);
        P.timer = nullptr;
        char text[200];
        summary(text, sizeof(text));
        ESP_LOGI(TAG, "scroll: %s", text);
        return;
    }
    // back and forth over up to 600 px, twice a second and a half, like a hand
    const float k = 0.5f - 0.5f * cosf(2 * float(M_PI) * float(elapsed) / 1.5e6f);
    const int32_t pos = int32_t(k * float(std::min<int32_t>(P.range, 600)));
    if (P.vertical) {
        lv_obj_scroll_to_y(P.target, pos, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_x(P.target, pos, LV_ANIM_OFF);
    }
}

void start_scroll(void *)
{
    if (P.timer) {
        return;
    }
    lv_obj_t *best = nullptr;
    int32_t range = 0;
    bool vertical = true;
    find(lv_screen_active(), &best, &range, &vertical);
    if (!best) {
        ESP_LOGW(TAG, "nothing to scroll on this screen");
        return;
    }
    P.target = best, P.range = range, P.vertical = vertical;
    P.origin = vertical ? lv_obj_get_scroll_y(best) : lv_obj_get_scroll_x(best);
    if (vertical) {
        lv_obj_scroll_to_y(best, 0, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_x(best, 0, LV_ANIM_OFF);
    }
    ESP_LOGI(TAG, "scrolling %s, %d px of range", vertical ? "vertically" : "horizontally", int(range));
    reset();
    P.scroll_start = esp_timer_get_time();
    P.timer = lv_timer_create(scroll_step, 1, nullptr);
}

// CPU time per task between method 13 (mark) and method 14 (report).
struct Mark {
    std::vector<TaskStatus_t> tasks;
    configRUN_TIME_COUNTER_TYPE total = 0;
} mark;

void snapshot(Mark &m)
{
    m.tasks.resize(uxTaskGetNumberOfTasks() + 4);
    const UBaseType_t n = uxTaskGetSystemState(m.tasks.data(), m.tasks.size(), &m.total);
    m.tasks.resize(n);
}

esp_err_t mark_rpc(const esp_iris_rpc_request_t *, uint8_t *, size_t, size_t *size, void *)
{
    snapshot(mark);
    *size = 0;
    return ESP_OK;
}

esp_err_t cpu_rpc(const esp_iris_rpc_request_t *, uint8_t *out, size_t capacity, size_t *size, void *)
{
    Mark now;
    snapshot(now);
    const double span = double(now.total - mark.total);
    struct Row {
        std::string name;
        double pct;
    };
    std::vector<Row> rows;
    for (const TaskStatus_t &t : now.tasks) {
        configRUN_TIME_COUNTER_TYPE before = 0;
        for (const TaskStatus_t &b : mark.tasks)
            if (b.xHandle == t.xHandle) {
                before = b.ulRunTimeCounter;
            }
        const double pct = span > 0 ? 100.0 * double(t.ulRunTimeCounter - before) / span : 0;
        if (pct >= 0.5) rows.push_back({t.pcTaskName, pct});
    }
    std::sort(rows.begin(), rows.end(), [](const Row & a, const Row & b) {
        return a.pct > b.pct;
    });
    std::string text = "% of one core:";
    for (const Row &r : rows) {
        char line[64];
        snprintf(line, sizeof(line), " %s %.0f;", r.name.c_str(), r.pct);
        text += line;
    }
    *size = std::min(capacity, text.size());
    memcpy(out, text.data(), *size);
    return ESP_OK;
}

std::atomic<bool> scroll_pending{false};

// ---- LVGL timer profile (RPC 15) --------------------------------------------------------
// Every LVGL timer's callback, timed for a few seconds (the display's refresh
// timer among them): where the LVGL task's time goes. Payload: seconds, which
// starts it; empty: the result (addresses, for addr2line on the ELF).
struct TimerProf {
    lv_timer_t *timer;
    lv_timer_cb_t cb;
    int64_t total, worst;
    int calls;
};
TimerProf prof[96];
int prof_n = 0;
std::atomic<int> prof_request{0};
int64_t prof_from = 0, prof_until = 0;
char prof_result[1000] = "not run";

void prof_trampoline(lv_timer_t *t)
{
    for (int i = 0; i < prof_n; ++i) {
        if (prof[i].timer != t) {
            continue;
        }
        TimerProf &p = prof[i];
        const int64_t a = esp_timer_get_time();
        p.cb(t);  // may delete t; p stays ours
        const int64_t d = esp_timer_get_time() - a;
        p.total += d;
        ++p.calls;
        if (d > p.worst) {
            p.worst = d;
        }
        return;
    }
}

void prof_poll()  // the LVGL task
{
    const int seconds = prof_request.exchange(0);
    const int64_t now = esp_timer_get_time();
    if (seconds > 0 && !prof_until) {
        prof_n = 0;
        for (lv_timer_t *t = lv_timer_get_next(nullptr); t && prof_n < int(sizeof(prof) / sizeof(*prof)); t = lv_timer_get_next(t)) {
            if (t->timer_cb == prof_trampoline) {
                continue;
            }
            prof[prof_n++] = {t, t->timer_cb, 0, 0, 0};
            t->timer_cb = prof_trampoline;
        }
        prof_from = now;
        prof_until = now + int64_t(seconds) * 1000000;
        strlcpy(prof_result, "running", sizeof(prof_result));
        return;
    }
    if (!prof_until || now < prof_until) {
        return;
    }
    // Put the callbacks back on the timers that still exist.
    for (lv_timer_t *t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
        if (t->timer_cb != prof_trampoline) {
            continue;
        }
        for (int i = 0; i < prof_n; ++i)
            if (prof[i].timer == t) {
                t->timer_cb = prof[i].cb;
            }
    }
    std::sort(prof, prof + prof_n, [](const TimerProf & a, const TimerProf & b) {
        return a.total > b.total;
    });
    const int64_t span = now - prof_from;
    int n = snprintf(prof_result, sizeof(prof_result), "%lld ms, %d timers; busiest:", (long long)(span / 1000), prof_n);
    for (int i = 0; i < prof_n && i < 12 && n > 0 && size_t(n) < sizeof(prof_result); ++i) {
        if (!prof[i].calls) {
            break;
        }
        n += snprintf(prof_result + n, sizeof(prof_result) - n, " 0x%08lx %.1f%% (%d calls, worst %lld ms);",
                      (unsigned long)(uintptr_t)prof[i].cb, 100.0 * double(prof[i].total) / double(span), prof[i].calls,
                      (long long)(prof[i].worst / 1000));
    }
    prof_until = 0;
}

esp_err_t prof_rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    if (request->payload_size >= 1) {
        prof_request = std::max<int>(1, request->payload[0]);
        strlcpy(prof_result, "running", sizeof(prof_result));
    }
    *size = std::min(capacity, strlen(prof_result));
    memcpy(out, prof_result, *size);
    return ESP_OK;
}

esp_err_t scroll_rpc(const esp_iris_rpc_request_t *, uint8_t *out, size_t capacity, size_t *size, void *)
{
    scroll_pending = true;  // picked up by an LVGL timer: no display lock here
    const char *reply = "started";
    *size = std::min(capacity, strlen(reply));
    memcpy(out, reply, *size);
    return ESP_OK;
}
}  // namespace

void perf_cpu_window(char *out, size_t capacity)
{
    static Mark last;
    Mark now;
    snapshot(now);
    const double span = double(now.total - last.total);
    struct Row {
        const char *name;
        double pct;
    };
    std::vector<Row> rows;
    for (const TaskStatus_t &t : now.tasks) {
        configRUN_TIME_COUNTER_TYPE before = 0;
        for (const TaskStatus_t &b : last.tasks)
            if (b.xHandle == t.xHandle) {
                before = b.ulRunTimeCounter;
            }
        const double pct = span > 0 ? 100.0 * double(t.ulRunTimeCounter - before) / span : 0;
        if (pct >= 2) rows.push_back({t.pcTaskName, pct});
    }
    std::sort(rows.begin(), rows.end(), [](const Row & a, const Row & b) {
        return a.pct > b.pct;
    });
    size_t n = 0;
    if (out && capacity) {
        out[0] = 0;
    }
    for (size_t i = 0; out && i < rows.size() && i < 8 && n + 1 < capacity; ++i) {
        const int w = snprintf(out + n, capacity - n, "%s%s %.0f", i ? ", " : "", rows[i].name, rows[i].pct);
        if (w < 0) {
            break;
        }
        n += size_t(w);
    }
    last = std::move(now);
}

void perf_start(lv_display_t *display)
{
    lv_display_add_event_cb(display, on_display, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(display, on_display, LV_EVENT_RENDER_READY, nullptr);
    lv_display_add_event_cb(display, on_display, LV_EVENT_FLUSH_WAIT_START, nullptr);
    lv_display_add_event_cb(display, on_display, LV_EVENT_FLUSH_WAIT_FINISH, nullptr);
    lv_timer_create([](lv_timer_t *) {
        if (scroll_pending.exchange(false)) {
            start_scroll(nullptr);
        }
        prof_poll();
    }, 100, nullptr);
    reset();
    esp_iris_rpc_register(RPC_SERVICE, 11, stats_rpc, nullptr);
    esp_iris_rpc_register(RPC_SERVICE, 12, scroll_rpc, nullptr);
    esp_iris_rpc_register(RPC_SERVICE, 13, mark_rpc, nullptr);
    esp_iris_rpc_register(RPC_SERVICE, 14, cpu_rpc, nullptr);
    esp_iris_rpc_register(RPC_SERVICE, 15, prof_rpc, nullptr);
}
