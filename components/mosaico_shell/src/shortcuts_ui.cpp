// SPDX-License-Identifier: Apache-2.0
// Shortcuts' screens (see shortcuts_internal.h), on the LVGL task and over
// any app (the top layer): a toast at the foot of the panel for a run, a card
// for a message step, a card that asks (Allow, Not now), and the editor: the
// list, a shortcut's page (what starts it, its steps and its last run's log,
// with Run or Stop, Test, on/off, its tile and Delete), and New (lights off,
// on or toggled for all lights or a room, or a scene, in two taps without a
// keyboard). Ifs, repeats and schedules are made in Web settings or by
// Swifty: the round screen shows them, in words.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "ArduinoJson.h"
#include "mui.h"
#include "shortcuts_internal.h"

namespace shortcuts {
namespace {
using namespace mui;
const Accent *const ACCENT_STYLES[] = {&VIOLET, &CYAN, &AMBER, &GREEN};

const Accent &accent_of(uint8_t a)
{
    return *ACCENT_STYLES[a % 4];
}

void fade_in(lv_obj_t *o)
{
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, o);
    lv_anim_set_values(&an, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&an, 160);
    lv_anim_set_exec_cb(&an, [](void *x, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(x), lv_opa_t(v), 0);
    });
    lv_anim_start(&an);
}

// A one-shot timer that deletes itself after its run.
lv_timer_t *once(lv_timer_cb_t cb, uint32_t ms)
{
    lv_timer_t *t = lv_timer_create(cb, ms, nullptr);
    lv_timer_set_repeat_count(t, 1);
    lv_timer_set_auto_delete(t, true);
    return t;
}

// ---- toast -------------------------------------------------------------------------------
struct {
    lv_obj_t *pill = nullptr;
    lv_timer_t *timer = nullptr;
    lv_timer_t *buzz = nullptr;
} T;

void close_toast()
{
    if (T.timer) {
        lv_timer_delete(T.timer);
        T.timer = nullptr;
    }
    if (T.pill) {
        lv_obj_delete(T.pill);
        T.pill = nullptr;
    }
}

// ---- message card ------------------------------------------------------------------------
struct {
    lv_obj_t *card = nullptr;
    lv_timer_t *timer = nullptr;
} M;

void close_message()
{
    if (M.timer) {
        lv_timer_delete(M.timer);
        M.timer = nullptr;
    }
    if (M.card) {
        lv_obj_delete(M.card);
        M.card = nullptr;
    }
}

// ---- the card that asks ------------------------------------------------------------------
struct {
    lv_obj_t *back = nullptr, *count = nullptr;
    lv_timer_t *timer = nullptr;
    int left = 0;
    std::function<void(bool)> done;
} A;

void answer(bool yes)
{
    std::function<void(bool)> done = std::move(A.done);
    A.done = nullptr;
    if (A.timer) {
        lv_timer_delete(A.timer);
        A.timer = nullptr;
    }
    if (A.back) {
        lv_obj_delete(A.back);
        A.back = A.count = nullptr;
    }
    if (done) {
        done(yes);
    }
}

// ---- the editor --------------------------------------------------------------------------
enum class Page : uint8_t { List, Detail, New };
struct {
    lv_obj_t *root = nullptr, *page = nullptr, *targets = nullptr, *scroll = nullptr;
    Page go_page = Page::List;
    uint32_t go_id = 0, delete_armed = 0;
    int kind = 0;           // New: 0 lights off, 1 on, 2 toggle, 3 a scene
    uint32_t shown_rev = 0;  // the revision the page shows
} E;

void editor_show(Page page, uint32_t id);

void editor_close()
{
    if (E.root) {
        lv_obj_delete(E.root);
        E.root = E.page = E.targets = E.scroll = nullptr;
    }
}

// Pages change after the tap that asked (the tapped object is on the old page).
void editor_go(Page page, uint32_t id = 0)
{
    E.go_page = page;
    E.go_id = id;
    lv_async_call([](void *) {
        if (E.root) {
            editor_show(E.go_page, E.go_id);
        }
    }, nullptr);
}

lv_obj_t *round_button(lv_obj_t *parent, int x, int y, const char *glyph, lv_event_cb_t cb, void *user = nullptr)
{
    lv_obj_t *b = box(parent, x, y, 48, 48, 24);
    lv_obj_set_style_bg_color(b, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    icon(b, glyph, 12, 12, 24, TEXT, 2.2f);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

lv_obj_t *scroller(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *s = box(parent, 40, y, PANEL - 80, h);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
    return s;
}

// A row in a list: a badge with a glyph, a title and a line under it.
lv_obj_t *row(lv_obj_t *list, int y, const char *glyph, const Accent &a, const std::string &title, const std::string &sub,
              lv_event_cb_t cb, void *user, uint32_t sub_color = TEXT3)
{
    const int w = PANEL - 80, h = sub.empty() ? 56 : 66;
    lv_obj_t *r = glass(list, 0, y, w, h, 20, a);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    press_fx(r);
    lv_obj_t *badge = box(r, 10, (h - 40) / 2, 40, 40, 20);
    lv_obj_set_style_bg_color(badge, lv_color_hex(a.glow), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    icon(badge, glyph, 9, 9, 22, a.ink, 2.2f);
    lv_obj_t *t = label(r, title.c_str(), 62, sub.empty() ? (h - 22) / 2 : 11, w - 76, TEXT, 17, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (!sub.empty()) {
        lv_obj_t *d = label(r, sub.c_str(), 62, 37, w - 76, sub_color, 13);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
    return r;
}

lv_obj_t *page_frame(const char *title, bool back)
{
    if (E.page) {
        lv_obj_delete(E.page);
    }
    E.targets = E.scroll = nullptr;
    E.page = box(E.root, 0, 0, PANEL, PANEL);
    lv_obj_t *t = label(E.page, title, 48, 44, back ? 300 : 320, TEXT, 26, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    round_button(E.page, PANEL - 96, 36, back ? "back" : "x", [](lv_event_t *e) {
        if (lv_event_get_user_data(e)) {
            editor_go(Page::List);
        } else {
            lv_async_call([](void *) {
                editor_close();
            }, nullptr);
        }
    }, back ? reinterpret_cast<void *>(1) : nullptr);
    return E.page;
}

// The line under a shortcut in the list: running, a failure, or what it does.
std::string list_line(const Shortcut &sc, uint32_t *color)
{
    const RunStatus st = status_of(sc.id);
    *color = TEXT3;
    if (st.running) {
        *color = OK;
        return "Running\xE2\x80\xA6";
    }
    if (st.ran && !st.ok && !st.dry) {
        *color = RED;
        return "Failed: " + st.message;
    }
    return summary_text(sc, action_words);
}

void list_page()
{
    lv_obj_t *p = page_frame("Shortcuts", false);
    const auto copy = list_copy();
    lv_obj_t *s = scroller(p, 100, 286);
    E.scroll = s;
    int y = 0;
    for (const auto &sc : copy) {
        uint32_t color;
        const std::string line = list_line(*sc, &color);
        lv_obj_t *r = row(s, y, sc->icon.c_str(), accent_of(sc->accent), sc->name, line, [](lv_event_t *e) {
            editor_go(Page::Detail, uint32_t(uintptr_t(lv_event_get_user_data(e))));
        }, reinterpret_cast<void *>(uintptr_t(sc->id)), color);
        if (!sc->enabled && sc->automatic()) {
            lv_obj_set_style_opa(r, LV_OPA_60, 0);
        }
        y += 76;
    }
    if (copy.empty()) {
        lv_obj_t *n = label(s, "No shortcuts yet. New makes one; Web settings and Swifty make longer ones, with "
                            "schedules, ifs and repeats.", 8, 8, PANEL - 96, TEXT2, 16);
        wrap_label(n);
    }
    lv_obj_t *add = fill_button(p, 96, 400, PANEL - 192, 50, "New shortcut", VIOLET, 17);
    lv_obj_add_event_cb(add, [](lv_event_t *) {
        editor_go(Page::New);
    }, LV_EVENT_CLICKED, nullptr);
    if (copy.size() >= MAX_COUNT) {
        lv_obj_add_state(add, LV_STATE_DISABLED);
    }
}

// A heading and its lines in the shortcut's page (a column: LVGL places them).
void section(lv_obj_t *column, const char *head, const std::string &lines, uint32_t color, int px)
{
    lv_obj_t *h = label(column, head, 0, 0, PANEL - 96, TEXT3, 12, true);
    wrap_label(h);  // as tall as its text and the gap above it
    lv_obj_set_style_pad_top(h, 10, 0);
    lv_obj_t *l = label(column, lines.c_str(), 0, 0, PANEL - 96, color, px);
    wrap_label(l);
    lv_obj_set_style_text_line_space(l, 3, 0);
}

std::string joined_lines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &l : lines) {
        out += (out.empty() ? "" : "\n") + l;
    }
    return out;
}

void detail_page(uint32_t id)
{
    std::shared_ptr<const Shortcut> sc = find(id);
    if (!sc) {
        return list_page();
    }
    lv_obj_t *p = page_frame(sc->name.c_str(), true);
    void *user = reinterpret_cast<void *>(uintptr_t(id));
    const Accent &a = accent_of(sc->accent);
    // On (its triggers start it) and its tile on the home screen.
    label(p, "On", 60, 101, 60, sc->automatic() ? TEXT : TEXT3, 16, true);
    lv_obj_t *on = toggle(p, 104, 96, sc->enabled, a);
    lv_obj_add_event_cb(on, [](lv_event_t *e) {
        std::string error;
        set_enabled(uint32_t(uintptr_t(lv_event_get_user_data(e))), toggle_on(lv_event_get_current_target_obj(e)), error);
    }, LV_EVENT_VALUE_CHANGED, user);
    label(p, "Tile on home screen", 196, 101, 170, TEXT, 16, true);
    lv_obj_t *tile = toggle(p, 368, 96, sc->tile(), a);
    lv_obj_add_event_cb(tile, [](lv_event_t *e) {
        std::string error;
        set_tile(uint32_t(uintptr_t(lv_event_get_user_data(e))), toggle_on(lv_event_get_current_target_obj(e)), error);
    }, LV_EVENT_VALUE_CHANGED, user);

    // What starts it, what it does and how it last went, in words.
    lv_obj_t *s = scroller(p, 140, 190);
    E.scroll = s;
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(s, 8, 0);
    std::vector<std::string> lines;
    for (const Trigger &t : sc->triggers) {
        lines.push_back(trigger_text(t));
    }
    if (lines.empty()) {
        lines.push_back("Nothing: run it here, from Swifty or another shortcut");
    }
    const std::string next = sc->enabled ? next_text(*sc) : "";
    if (!next.empty()) {
        lines.push_back("Next: " + next);
    } else if (!sc->enabled && sc->automatic()) {
        lines.push_back("Off: only a tap starts it");
    }
    section(s, "STARTS", joined_lines(lines), TEXT, 16);
    lines.clear();
    steps_lines(sc->steps, lines, action_words);
    section(s, "DOES", joined_lines(lines), TEXT2, 15);
    const RunStatus st = status_of(id);
    if (st.running || st.ran) {
        std::string head = st.running ? "Running now" : std::string(st.ok ? "OK" : "Failed") + (st.dry ? " (test)" : "");
        if (!st.running && st.at) {
            char t[16];
            tm local;
            localtime_r(&st.at, &local);
            strftime(t, sizeof(t), " %a %H:%M", &local);
            head += t;
        }
        if (!st.running) {
            head += std::string(" \xC2\xB7 ") + source_name(st.source) + "\n" + st.message;
        }
        section(s, "LAST RUN", head, st.running ? OK : st.ok ? TEXT : RED, 15);
        if (!st.running && !st.log.empty()) {
            lv_obj_t *log = label(s, joined_lines(st.log).c_str(), 0, 0, PANEL - 96, TEXT3, 13);
            wrap_label(log);
            lv_obj_set_style_pad_top(log, 4, 0);
        }
    }
    lv_obj_t *pad = box(s, 0, 0, 10, 12);  // room under the last line
    (void)pad;

    lv_obj_t *run = st.running ? field_button(p, 48, 340, 184, 48, "Stop", 17, RED)
                    : fill_button(p, 48, 340, 184, 48, "Run", a, 17);
    lv_obj_add_event_cb(run, [](lv_event_t *e) {
        const uint32_t id = uint32_t(uintptr_t(lv_event_get_user_data(e)));
        if (status_of(id).running) {
            stop(id);
        } else {
            start(id, false, Source::Editor);
        }
    }, LV_EVENT_CLICKED, user);
    lv_obj_t *test = field_button(p, 248, 340, 184, 48, "Test (dry run)", 16);
    lv_obj_add_event_cb(test, [](lv_event_t *e) {
        start(uint32_t(uintptr_t(lv_event_get_user_data(e))), true, Source::Editor);
    }, LV_EVENT_CLICKED, user);
    if (st.running) {
        lv_obj_add_state(test, LV_STATE_DISABLED);
    }
    lv_obj_t *del = field_button(p, 96, 398, PANEL - 192, 44, E.delete_armed == id ? "Tap again to delete" : "Delete", 16, RED);
    lv_obj_add_event_cb(del, [](lv_event_t *e) {
        const uint32_t id = uint32_t(uintptr_t(lv_event_get_user_data(e)));
        lv_obj_t *button = lv_event_get_current_target_obj(e);
        if (E.delete_armed != id) {
            E.delete_armed = id;  // a second tap deletes
            lv_obj_t *text = lv_obj_get_child(button, 0);
            if (text) {
                set_text(text, "Tap again to delete");
            }
            return;
        }
        std::shared_ptr<const Shortcut> sc = find(id);
        std::string name;
        if (sc && erase(id, name)) {
            ui_toast(name, "x", sc->accent, Feedback::Done, sc->tile() ? "Deleted, with its tile" : "Deleted");
        }
        editor_go(Page::List);
    }, LV_EVENT_CLICKED, user);
}

// New: what (lights off, on, toggled, a scene), then which.
const char *const KIND_LABELS[] = {"Off", "On", "Toggle", "Scene"};
std::vector<std::string> target_keys;  // the rows' user data (kept while they are shown)

void target_tapped(lv_event_t *e)
{
    // The row's user data: "<target>\x1f<title>" (target_keys holds it).
    const char *packed = static_cast<const char *>(lv_event_get_user_data(e));
    const char *sep = strchr(packed, '\x1f');
    if (!sep) {
        return;
    }
    const std::string target(packed, sep), title(sep + 1);
    Shortcut sc;
    Node n;
    n.kind = Do::Home;
    n.act.kind = Do::Home;
    if (E.kind == 3) {
        n.act.verb = "scene";
        n.act.value = target;
        sc.name = title;
        sc.icon = "house", sc.accent = 1;
    } else {
        n.act.verb = E.kind == 0 ? "lights_off" : E.kind == 1 ? "lights_on" : "lights_toggle";
        n.act.target = target;
        sc.name = title + (E.kind == 0 ? " lights off" : E.kind == 1 ? " lights on" : " lights");
        if (target == "all") {
            sc.name = E.kind == 0 ? "All lights off" : E.kind == 1 ? "All lights on" : "All lights";
        }
        sc.icon = E.kind == 0 ? "power" : "bulb";
        sc.accent = E.kind == 0 ? 0 : 2;
    }
    sc.name = trimmed(sc.name, MAX_NAME);
    sc.triggers.push_back(Trigger());
    sc.steps.push_back(n);
    if (name_taken(sc.name)) {
        ui_toast(sc.name, sc.icon, sc.accent, Feedback::Failed, "There is one with that name");
        return;
    }
    std::string error;
    if (!add_made(sc, error)) {
        ui_toast(sc.name, sc.icon, sc.accent, Feedback::Failed, error);
        return;
    }
    ui_toast(sc.name, sc.icon, sc.accent, Feedback::Done, "Added to the home screen");
    editor_go(Page::List);
}

void fill_targets()
{
    if (!E.targets) {
        return;
    }
    lv_obj_clean(E.targets);
    target_keys.clear();
    std::vector<std::pair<std::string, std::string>> choices;  // target, title
    std::string note;
    JsonDocument doc;
    deserializeJson(doc, home_choices());
    if (!(doc["ready"] | false)) {
        note = "Swifty Home has not answered yet (pair it in Home).";
    } else if (E.kind == 3) {
        for (JsonObjectConst sc : doc["scenes"].as<JsonArrayConst>()) {
            choices.push_back({sc["id"] | "", sc["name"] | (sc["id"] | "")});
        }
        if (choices.empty()) {
            note = "Swifty Home has no scenes.";
        }
    } else {
        if (doc["lights"].as<JsonArrayConst>().size()) {
            choices.push_back({"all", "All lights"});
        }
        for (JsonObjectConst r : doc["rooms"].as<JsonArrayConst>())
            if ((r["lights"] | 0) > 0) {
                choices.push_back({r["id"] | "", r["name"] | ""});
            }
        if (choices.empty()) {
            note = "Swifty Home has no lights.";
        }
    }
    target_keys.reserve(choices.size());
    int y = 0;
    const Accent &a = E.kind == 3 ? CYAN : E.kind == 0 ? VIOLET : AMBER;
    const char *glyph = E.kind == 3 ? "house" : E.kind == 0 ? "power" : "bulb";
    for (const auto &[target, title] : choices) {
        target_keys.push_back(target + '\x1f' + title);
        row(E.targets, y, glyph, a, title, "", target_tapped, const_cast<char *>(target_keys.back().c_str()));
        y += 64;
    }
    if (!note.empty()) {
        lv_obj_t *n = label(E.targets, note.c_str(), 8, 8, PANEL - 96, TEXT2, 16);
        wrap_label(n);
    }
}

void new_page()
{
    lv_obj_t *p = page_frame("New shortcut", true);
    lv_obj_t *c = capsule(p, 40, 102, PANEL - 80, 46, KIND_LABELS, 4, E.kind, VIOLET, 16);
    lv_obj_add_event_cb(c, [](lv_event_t *e) {
        E.kind = capsule_selected(lv_event_get_current_target_obj(e));
        fill_targets();
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    E.targets = scroller(p, 164, 238);
    fill_targets();
    label(p, "Schedules, ifs and repeats: Web settings, or ask Swifty.", 48, 416, PANEL - 96, TEXT3, 13, false,
          LV_TEXT_ALIGN_CENTER);
}

void editor_show(Page page, uint32_t id)
{
    if (page != E.go_page || id != E.go_id) {
        E.delete_armed = 0;
    }
    E.go_page = page;  // the page in view (Back reads it)
    E.go_id = id;
    E.shown_rev = revision();
    if (page == Page::Detail) {
        detail_page(id);
    } else if (page == Page::New) {
        new_page();
    } else {
        list_page();
    }
    if (T.pill) {
        lv_obj_move_foreground(T.pill);  // a toast stays over the sheet
    }
}
}  // namespace

// ---- the screens, for the engine ---------------------------------------------------------
// A pill at the foot of the panel, over any app: the shortcut's icon (a tick
// or a cross once it ends), its name and how it went.
void ui_toast(const std::string &name, const std::string &glyph_name, uint8_t accent, Feedback state, const std::string &detail)
{
    close_toast();
    board_wake();
    const Accent &a = accent_of(accent);
    const bool two_lines = !detail.empty();
    const int h = two_lines ? 78 : 60, w = 400, x = (PANEL - w) / 2, y = PANEL - h - 44;
    lv_obj_t *pill = glass(lv_layer_top(), x, y, w, h, h / 2, a);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    T.pill = pill;
    lv_obj_t *badge = box(pill, 10, (h - 44) / 2, 44, 44, 22);
    const uint32_t ring = state == Feedback::Failed ? RED : state == Feedback::Done ? OK : a.glow;
    lv_obj_set_style_bg_color(badge, lv_color_hex(ring), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    const char *glyph = state == Feedback::Failed ? "x" : state == Feedback::Done ? "check" : glyph_name.c_str();
    icon(badge, glyph, 10, 10, 24, state == Feedback::Running ? a.ink : 0x06200F, 2.2f);
    const int tx = 66, tw = w - tx - 22;
    std::string title = name;
    if (state == Feedback::Running) {
        title += "\xE2\x80\xA6";
    }
    lv_obj_t *t = label(pill, title.c_str(), tx, two_lines ? 12 : (h - 24) / 2, tw, TEXT, 18, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (two_lines) {
        lv_obj_t *d = label(pill, detail.c_str(), tx, 40, tw, state == Feedback::Failed ? RED : TEXT2, 14);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
    }
    fade_in(pill);
    if (state != Feedback::Running) {
        T.timer = once([](lv_timer_t *) {
            T.timer = nullptr;  // deleted after this run
            if (T.pill) {
                lv_obj_delete(T.pill);
                T.pill = nullptr;
            }
        }, state == Feedback::Failed ? 5000 : 2500);
    }
}

void ui_buzz(bool failed)
{
    board_buzz();
    if (failed && !T.buzz) {
        T.buzz = once([](lv_timer_t *) {
            T.buzz = nullptr;
            board_buzz();
        }, 220);
    }
}

// A card near the top, over any app, for a message step: the shortcut's name
// and the text. A tap closes it; it goes by itself after 8 s (12 s when long).
void ui_message(const std::string &title, const std::string &text, uint8_t accent, bool buzz, bool chime)
{
    close_message();
    board_wake();
    const Accent &a = accent_of(accent);
    const int w = 408, x = (PANEL - w) / 2, y = 60;
    lv_obj_t *c = glass(lv_layer_top(), x, y, w, 120, 28, a);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(c, 18, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, [](lv_event_t *) {
        lv_async_call([](void *) {
            close_message();
        }, nullptr);
    }, LV_EVENT_CLICKED, nullptr);
    M.card = c;
    lv_obj_t *badge = box(c, 18, 18, 40, 40, 20);
    lv_obj_set_style_bg_color(badge, lv_color_hex(a.glow), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    icon(badge, "bell", 9, 9, 22, a.ink, 2.2f);
    lv_obj_t *t = label(c, title.c_str(), 72, 28, w - 96, TEXT3, 14, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *body = label(c, text.c_str(), 22, 70, w - 44, TEXT, 20, true);
    wrap_label(body);
    fade_in(c);
    if (T.pill) {
        lv_obj_move_foreground(T.pill);
    }
    if (chime) {
        board_chime();
    }
    if (buzz || chime) {
        ui_buzz(false);
    }
    M.timer = once([](lv_timer_t *) {
        M.timer = nullptr;
        if (M.card) {
            lv_obj_delete(M.card);
            M.card = nullptr;
        }
    }, text.size() > 60 ? 12000 : 8000);
}

// A card that asks, over a dimmed screen that takes the taps meant for the
// app under it: what wants to happen, Not now and Allow, and how long it
// waits. Nothing happens unless Allow is tapped.
void ui_ask(const std::string &title, const std::string &body, const std::string &allow, int timeout_s,
            std::function<void(bool)> done)
{
    answer(false);  // one at a time: the one before is a no
    board_wake();
    A.done = std::move(done);
    A.left = timeout_s;
    lv_obj_t *back = box(lv_layer_top(), 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(back, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(back, 170, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    A.back = back;
    lv_obj_t *c = glass(back, 36, 72, 408, 336, 30, AMBER);
    lv_obj_t *badge = box(c, 24, 24, 48, 48, 24);
    lv_obj_set_style_bg_color(badge, lv_color_hex(AMBER.glow), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    icon(badge, "key", 12, 12, 24, AMBER.ink, 2.2f);
    label(c, "Allow?", 88, 22, 290, TEXT3, 14, true);
    lv_obj_t *t = label(c, title.c_str(), 88, 42, 296, TEXT, 21, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *text = label(c, body.c_str(), 24, 92, 360, TEXT2, 15);
    wrap_label(text);
    lv_obj_set_style_max_height(text, 140, 0);  // about seven lines; the rest is cut
    A.count = label(c, "", 24, 238, 360, TEXT3, 13);
    lv_obj_t *no = field_button(c, 24, 262, 170, 50, "Not now", 16);
    lv_obj_add_event_cb(no, [](lv_event_t *) {
        lv_async_call([](void *) {
            answer(false);
        }, nullptr);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *yes = fill_button(c, 210, 262, 174, 50, allow.c_str(), AMBER, 17);
    lv_obj_add_event_cb(yes, [](lv_event_t *) {
        lv_async_call([](void *) {
            answer(true);
        }, nullptr);
    }, LV_EVENT_CLICKED, nullptr);
    auto tick = [](lv_timer_t *) {
        if (A.left <= 0) {
            answer(false);
            return;
        }
        char t[48];
        snprintf(t, sizeof(t), "Not now in %d s if nobody answers", A.left--);
        set_text(A.count, t);
    };
    tick(nullptr);
    A.timer = lv_timer_create(tick, 1000, nullptr);
    fade_in(back);
    ui_buzz(false);
}

void ui_editor_open()
{
    if (E.root) {
        return;
    }
    E.root = box(lv_layer_top(), 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(E.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(E.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(E.root, LV_OBJ_FLAG_CLICKABLE);  // takes the taps meant for the app under it
    E.kind = 0;
    editor_show(Page::List, 0);
    if (A.back) {
        lv_obj_move_foreground(A.back);  // a card that asks stays on top
    }
}

bool ui_editor_back()
{
    if (!E.root) {
        return false;
    }
    if (E.go_page != Page::List) {
        editor_show(Page::List, 0);
    } else {
        editor_close();
    }
    return true;
}

void ui_tick()
{
    // The list and a shortcut's page follow runs and changes made elsewhere
    // (Web settings, Swifty), keeping where they were scrolled to.
    if (!E.root || E.go_page == Page::New || E.shown_rev == revision()) {
        return;
    }
    const int32_t y = E.scroll ? lv_obj_get_scroll_y(E.scroll) : 0;
    set_entry_animations(false);
    editor_show(E.go_page, E.go_id);
    set_entry_animations(true);
    if (E.scroll && y) {
        lv_obj_scroll_to_y(E.scroll, y, LV_ANIM_OFF);
    }
    if (A.back) {
        lv_obj_move_foreground(A.back);
    }
    if (M.card) {
        lv_obj_move_foreground(M.card);
    }
}

}  // namespace shortcuts
