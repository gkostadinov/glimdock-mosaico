// SPDX-License-Identifier: Apache-2.0
// The profiles' screens (see profiles_ui.h). Everything is placed inside the
// round panel's circle: titles and buttons centred, the PIN pad's corners
// inside it, lists in a centred column.
#include "profiles_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "face_id.h"
#include "mui.h"
#include "profiles.h"

namespace {
using namespace mui;

constexpr int SLOTS = PROFILES_MAX + 1;
// Lists: a centred column, inside the circle down to y 410; below that a
// fade over the panel's foot shows that the list goes on.
constexpr int COLUMN_X = 70, COLUMN_W = 340, FADE_Y = 400;
constexpr int LIST_Y = 108;                   // where a page's scrolling part starts
constexpr uint32_t FACE_TIMEOUT_MS = 8000;

// The profiles' colours (profile_t.color), the last Guest's grey.
const Accent ROSE = {0xFFA3B5, 0xC8325A, 0xF0577A, 0x2A0610, 0x3A1420, 0x1A080D};
const Accent CORAL = {0xFFB199, 0xE0533D, 0xFF8A70, 0x2A0A04, 0x3A1610, 0x1A0906};
const Accent SKY = {0x7DF3FF, 0x2B5CF2, 0x35B2FF, 0x041428, 0x12305A, 0x060E1C};
const Accent SLATE = {0xB8B8C0, 0x3A3A40, 0x8A8A92, 0x101012, 0x2A2A2E, 0x151517};
const Accent *const PALETTE[PROFILE_COLORS] = {&VIOLET, &CYAN, &AMBER, &GREEN, &ROSE, &CORAL, &SKY, &SLATE};

const Accent &accent_of(const profile_t &p)
{
    return *PALETTE[p.color % PROFILE_COLORS];
}

enum class Page : uint8_t { None, Pick, Face, Pin, Intro, List, Edit, Rename, Enroll };
enum class PinFor : uint8_t { SignIn, Set, Setup };

struct {
    ProfilesUiHooks hooks;
    lv_obj_t *root = nullptr, *page = nullptr, *scroller = nullptr;
    Page shown = Page::None, go = Page::None;
    Page back_to = Page::Pick;  // where the face and PIN pages return
    uint8_t target = 0;         // the profile signed into or edited
    PinFor pin_for = PinFor::SignIn;
    char digits[PROFILE_PIN_MAX + 1] = {};
    char first[PROFILE_PIN_MAX + 1] = {};  // a new PIN typed once, waiting for the second time
    lv_obj_t *dots = nullptr, *status = nullptr, *area = nullptr;
    lv_timer_t *timer = nullptr;
    bool busy = false;          // a hash is running
    bool face_running = false;
    bool full_after_face = false;  // the full mode switch asked for a face: on once it is added
    bool saving = false;        // our own save: the change listener leaves the page alone
    bool off_armed = false, delete_armed = false;
    uint32_t generation = 0;    // bumps with every page: late callbacks see it moved on
    int32_t keep_scroll = -1;   // the scroll to restore on the next build
    std::string message;        // a line for the next PIN page's status
    uint32_t message_color = TEXT2;
    lv_obj_t *note = nullptr;
    lv_timer_t *note_timer = nullptr;
    std::vector<lv_obj_t *> badges;
} U;

void wipe(char *s, size_t n)
{
    volatile char *p = s;
    for (size_t i = 0; i < n; ++i) {
        p[i] = 0;
    }
}

// The half-width of the circle at height y.
int half_chord(int y)
{
    const int d = y - PANEL / 2;
    return int(sqrtf(float(PANEL * PANEL / 4 - d * d)));
}

bool manage()
{
    return profiles_can(PROFILE_CAP_SETTINGS_PROFILES);
}

void show(Page page);

// Pages change after the tap that asked: the tapped object is on the old page.
void go(Page page)
{
    U.go = page;
    lv_async_call([](void *) {
        if (U.root) {
            show(U.go);
        }
    }, nullptr);
}

// Closing from a tap on the sheet: after the tap (its object goes with the sheet).
void close_later()
{
    lv_async_call([](void *) {
        profiles_ui_close();
    }, nullptr);
}

void on(lv_obj_t *o, lv_event_cb_t cb, void *data = nullptr, lv_event_code_t code = LV_EVENT_CLICKED)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, cb, code, data);
}

void *as_data(intptr_t v)
{
    return reinterpret_cast<void *>(v);
}

intptr_t data_of(lv_event_t *e)
{
    return reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
}

lv_obj_t *round_button(lv_obj_t *parent, int x, int y, int size, const char *glyph, lv_event_cb_t cb, void *data = nullptr)
{
    lv_obj_t *b = box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    press_fx(b);
    const int g = size * 11 / 20;
    icon(b, glyph, (size - g) / 2, (size - g) / 2, g, TEXT, 2.2f);
    on(b, cb, data);
    return b;
}

lv_obj_t *centered(lv_obj_t *parent, const char *text, int y, uint32_t color, int px, bool semi = false, int w = 320)
{
    return label(parent, text, (PANEL - w) / 2, y, w, color, px, semi, LV_TEXT_ALIGN_CENTER);
}

lv_obj_t *wrapped(lv_obj_t *parent, const char *text, int x, int y, int w, uint32_t color, int px,
                  lv_text_align_t align = LV_TEXT_ALIGN_CENTER)
{
    lv_obj_t *l = label(parent, text, x, y, w, color, px, false, align);
    wrap_label(l);
    return l;
}

// The first character of a name, as a capital when it is a letter.
std::string initial(const char *name)
{
    if (!name || !*name) {
        return "?";
    }
    const unsigned char c = uint8_t(name[0]);
    if (c < 0x80) {
        return std::string(1, char(c >= 'a' && c <= 'z' ? c - 32 : c));
    }
    size_t n = 1;
    while (name[n] && (uint8_t(name[n]) & 0xC0) == 0x80) {
        ++n;
    }
    return std::string(name, n);
}

// A profile's round avatar: its colour, its initial (Guest: a person).
lv_obj_t *avatar(lv_obj_t *parent, int x, int y, int size, const profile_t &p, bool ring = false)
{
    const Accent &a = accent_of(p);
    lv_obj_t *o = box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    style_fill(o, a, false);
    if (p.id == PROFILE_GUEST) {
        const int g = size * 11 / 20;
        icon(o, "person", (size - g) / 2, (size - g) / 2, g, a.ink, std::max(1.8f, size / 24.0f));
    } else {
        const int px = std::max(12, size * 9 / 20);
        const lv_font_t *f = font(px, true);
        label(o, initial(p.name).c_str(), 0, (size - lv_font_get_line_height(f)) / 2, size, a.ink, px, true,
              LV_TEXT_ALIGN_CENTER);
    }
    if (ring) {
        lv_obj_set_style_border_width(o, std::max(2, size / 24), 0);
        lv_obj_set_style_border_color(o, lv_color_hex(TEXT), 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    }
    return o;
}

const char *signin_words(const profile_t &p)
{
    if (p.id == PROFILE_GUEST) {
        return "No sign-in";
    }
    if (p.elevated) {
        return "Face and PIN \xC2\xB7 full mode";
    }
    switch (p.signin) {
    case PROFILE_SIGNIN_PIN: return "PIN";
    case PROFILE_SIGNIN_FACE: return "Face";
    case PROFILE_SIGNIN_FACE_PIN: return "Face and PIN";
    default: return "No sign-in";
    }
}

const char *signin_glyph(const profile_t &p)
{
    if (p.id == PROFILE_GUEST || (!p.elevated && p.signin == PROFILE_SIGNIN_NONE)) {
        return nullptr;
    }
    return p.elevated || p.signin == PROFILE_SIGNIN_FACE_PIN ? "lock" : p.signin == PROFILE_SIGNIN_FACE ? "camera" : "key";
}

void stop_face()
{
    if (U.face_running) {
        U.face_running = false;
        face_id_cancel();
    }
}

void stop_timer()
{
    if (U.timer) {
        lv_timer_delete(U.timer);
        U.timer = nullptr;
    }
}

// A fresh page on the root: the old one, its timers and a face scan go.
lv_obj_t *frame(Page page)
{
    stop_face();
    stop_timer();
    if (U.page) {
        lv_obj_delete(U.page);
    }
    U.page = box(U.root, 0, 0, PANEL, PANEL);
    U.scroller = U.dots = U.status = U.area = nullptr;
    U.shown = page;
    ++U.generation;
    return U.page;
}

void ensure_root()
{
    if (!U.root) {
        U.root = box(lv_layer_top(), 0, 0, PANEL, PANEL);
        lv_obj_set_style_bg_color(U.root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(U.root, LV_OPA_COVER, 0);
        lv_obj_add_flag(U.root, LV_OBJ_FLAG_CLICKABLE);  // the app below takes no taps meanwhile
    }
    lv_obj_move_foreground(U.root);
}

// A settings page: back at the left of a centred title, and a scrolling
// column below it.
lv_obj_t *settings_frame(Page page, const char *title, lv_event_cb_t back)
{
    lv_obj_t *pg = frame(page);
    round_button(pg, 100, 50, 44, "back", back);
    lv_obj_t *t = label(pg, title, 152, 58, 220, TEXT, 24, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    U.scroller = box(pg, 0, LIST_Y, PANEL, PANEL - LIST_Y);
    lv_obj_add_flag(U.scroller, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(U.scroller, LV_OBJ_FLAG_CLICKABLE);  // a drag on text or a gap scrolls too
    lv_obj_set_scroll_dir(U.scroller, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.scroller, LV_SCROLLBAR_MODE_OFF);
    // Rows fade out where the circle narrows (taps pass through the fade).
    lv_obj_t *fade = box(pg, 0, FADE_Y, PANEL, PANEL - FADE_Y);
    lv_obj_set_style_bg_color(fade, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(fade, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(fade, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_stop(fade, 0, 0);
    lv_obj_set_style_bg_grad_stop(fade, 200, 0);
    lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
    return U.scroller;
}

// Room past the bottom, then the scroll kept from before a rebuild.
void finish_scroller(int y)
{
    box(U.scroller, 0, y, 1, 120);
    if (U.keep_scroll > 0) {
        lv_obj_update_layout(U.scroller);
        lv_obj_scroll_to_y(U.scroller, U.keep_scroll, LV_ANIM_OFF);
    }
    U.keep_scroll = -1;
}

void rebuild_keeping_scroll()
{
    U.keep_scroll = U.scroller ? lv_obj_get_scroll_y(U.scroller) : -1;
    go(U.shown);
}

void section(lv_obj_t *parent, const char *title, int y)
{
    label(parent, title, COLUMN_X + 8, y, COLUMN_W - 16, TEXT3, 14, true);
}

// ---- the note at the foot -------------------------------------------------------------------
void close_note()
{
    if (U.note_timer) {
        lv_timer_delete(U.note_timer);
        U.note_timer = nullptr;
    }
    if (U.note) {
        lv_obj_delete(U.note);
        U.note = nullptr;
    }
}

// ---- the lock screen ------------------------------------------------------------------------
void choose(uint8_t id);

void pick_cb(lv_event_t *e)
{
    choose(uint8_t(data_of(e)));
}

void pick_page()
{
    lv_obj_t *pg = frame(Page::Pick);
    profile_t all[SLOTS];
    const int count = profiles_list(all, SLOTS);
    // Those who sign in first, Guest last.
    std::vector<profile_t> list;
    for (int i = 0; i < count; ++i)
        if (all[i].id != PROFILE_GUEST) {
            list.push_back(all[i]);
        }
    for (int i = 0; i < count; ++i)
        if (all[i].id == PROFILE_GUEST) {
            list.push_back(all[i]);
        }
    const int n = int(list.size());
    // One row up to four, two rows up to eight, three rows of three for nine:
    // each row as wide as the circle allows there.
    const int rows = n <= 4 ? 1 : n <= 8 ? 2 : 3;
    const int size = rows == 1 ? 84 : rows == 2 ? 72 : 64, cell_h = size + 30, pitch = rows == 3 ? 104 : size + 30;
    const int block = rows * cell_h + (rows - 1) * 6;
    const int top = rows == 3 ? 92 : 236 - block / 2;
    centered(pg, "Who's using it?", rows == 3 ? 38 : top - 84, TEXT, 24, true);
    profile_t now{};
    if (rows < 3 && profiles_get(profiles_current(), &now)) {
        char line[64];
        snprintf(line, sizeof(line), "%s now%s", now.name, profiles_elevated() ? " \xC2\xB7 full mode" : "");
        centered(pg, line, top - 50, TEXT3, 15);
    }
    int index = 0;
    for (int r = 0; r < rows; ++r) {
        const int in_row = rows == 1 ? n : rows == 2 ? (r == 0 ? (n + 1) / 2 : n / 2) : 3;
        const int y = top + r * (cell_h + 6);
        const int x0 = PANEL / 2 - (in_row * pitch) / 2;
        for (int c = 0; c < in_row && index < n; ++c, ++index) {
            const profile_t &p = list[size_t(index)];
            lv_obj_t *cell = box(pg, x0 + c * pitch, y, pitch, cell_h);
            const bool current = p.id == profiles_current();
            avatar(cell, (pitch - size) / 2, 0, size, p, current);
            if (const char *g = signin_glyph(p)) {
                const int b = std::max(22, size / 3);
                lv_obj_t *badge = box(cell, (pitch + size) / 2 - b + 2, size - b + 2, b, b, LV_RADIUS_CIRCLE);
                lv_obj_set_style_bg_color(badge, lv_color_hex(p.elevated ? AMBER.glow : FIELD), 0);
                lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(badge, 2, 0);
                lv_obj_set_style_border_color(badge, lv_color_black(), 0);
                const int gs = b * 3 / 5;
                icon(badge, g, (b - gs) / 2, (b - gs) / 2, gs, p.elevated ? 0x2A1A05 : TEXT, 2.0f);
            }
            lv_obj_t *name = label(cell, p.name, 0, size + 6, pitch, current ? TEXT : TEXT2, 15, true, LV_TEXT_ALIGN_CENTER);
            lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
            press_fx(cell);
            on(cell, pick_cb, as_data(p.id));
        }
    }
    // Lock (when someone is signed in) and Cancel, side by side at the foot.
    profiles_settings_t s{};
    profiles_get_settings(&s);
    const bool can_lock = profiles_current() != s.default_id;
    const int by = rows == 3 ? 392 : std::min(392, top + block + 40);
    const int bw = 132;
    if (can_lock) {
        lv_obj_t *lock = fill_button(pg, PANEL / 2 - bw - 6, by, bw, 46, "Lock", VIOLET, 17);
        on(lock, [](lv_event_t *) {
            profiles_lock();
            close_later();
        });
    }
    lv_obj_t *cancel = field_button(pg, can_lock ? PANEL / 2 + 6 : (PANEL - bw) / 2, by, bw, 46, "Cancel", 17, TEXT2);
    on(cancel, [](lv_event_t *) {
        close_later();
    });
    stagger(pg, 0, 14);
}

// ---- the face scan --------------------------------------------------------------------------
void face_page();
void pin_page();

void signed_in(uint8_t id)
{
    profile_t p{};
    profiles_get(id, &p);
    char text[64];
    snprintf(text, sizeof(text), "Hi, %s%s", p.name, profiles_elevated() ? " \xC2\xB7 full mode" : "");
    close_later();
    profiles_ui_note(text);
}

// The face matched (or none was needed): the PIN next, or in.
void after_face()
{
    profile_t p{};
    if (!profiles_get(U.target, &p)) {
        go(Page::Pick);
        return;
    }
    const uint8_t signin = p.elevated ? uint8_t(PROFILE_SIGNIN_FACE_PIN) : p.signin;
    if (signin == PROFILE_SIGNIN_FACE_PIN || signin == PROFILE_SIGNIN_PIN) {
        U.pin_for = PinFor::SignIn;
        U.message = signin == PROFILE_SIGNIN_FACE_PIN ? "Face matched. Now your PIN." : "";
        U.message_color = OK;
        go(Page::Pin);
        return;
    }
    const profile_result_t r = profiles_sign_in(U.target);
    if (r == PROFILE_OK) {
        signed_in(U.target);
    } else {
        profiles_ui_note(profiles_result_text(r));
        go(Page::Pick);
    }
}

void face_status(const char *text, uint32_t color)
{
    if (U.status) {
        set_text(U.status, text);
        set_text_color(U.status, lv_color_hex(color));
    }
}

void face_done(face_id_match_t match, void *user)
{
    if (reinterpret_cast<uintptr_t>(user) != U.generation || U.shown != Page::Face) {
        return;  // the page moved on meanwhile
    }
    U.face_running = false;
    if (match.result == FACE_ID_OK && match.profile == U.target) {
        profiles_face_matched(U.target);
        after_face();
        return;
    }
    switch (match.result) {
    case FACE_ID_OK:
    case FACE_ID_NO_MATCH:
        profiles_face_failed(U.target);
        if (U.hooks.haptic) {
            U.hooks.haptic(true);
        }
        if (const uint32_t w = profiles_wait_s(U.target)) {
            char text[64];
            snprintf(text, sizeof(text), "Not recognised. Try again in %u s", unsigned(w));
            face_status(text, WARN);
        } else {
            face_status("Not recognised. Tap to try again", RED);
        }
        break;
    case FACE_ID_TIMEOUT:  // matched, but the head never moved (face ID's liveness)
        face_status("Turn your head slightly. Tap to try again", WARN);
        break;
    case FACE_ID_NO_FACE:
        face_status("No face seen. Tap to try again", WARN);
        break;
    case FACE_ID_MANY_FACES:
        face_status("One face at a time. Tap to try again", WARN);
        break;
    case FACE_ID_CANCELLED:
        break;
    case FACE_ID_NO_CAMERA:
        go(Page::Face);  // shows that it needs the camera module
        break;
    default:
        face_status("The camera is busy. Tap to try again", WARN);
        break;
    }
}

void start_face()
{
    if (U.face_running) {
        return;
    }
    if (const uint32_t w = profiles_wait_s(U.target)) {
        char text[64];
        snprintf(text, sizeof(text), "Too many tries. Wait %u s", unsigned(w));
        face_status(text, WARN);
        return;
    }
    U.face_running = face_id_identify_start(FACE_TIMEOUT_MS, face_done, reinterpret_cast<void *>(uintptr_t(U.generation)));
    face_status(U.face_running ? "Hold still\xE2\x80\xA6" : "The camera is busy. Tap to try again", U.face_running ? TEXT2 : WARN);
}

void face_page()
{
    lv_obj_t *pg = frame(Page::Face);
    profile_t p{};
    profiles_get(U.target, &p);
    if (!face_id_available()) {
        centered(pg, p.name, 70, TEXT, 24, true);
        lv_obj_t *c = box(pg, 170, 116, 140, 140, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(c, lv_color_hex(CARD), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(EDGE), 0);
        icon(c, "camera", 38, 38, 64, TEXT3, 2.4f);
        centered(pg, "Face sign-in needs the camera module", 280, TEXT, 18, true, 340);
        wrapped(pg, "Plug the camera module in, then try again.", 90, 312, 300, TEXT2, 15);
        lv_obj_t *back = field_button(pg, (PANEL - 150) / 2, 380, 150, 46, "Back", 17, TEXT2);
        on(back, [](lv_event_t *) {
            go(U.back_to);
        });
        stagger(pg, 0, 14);
        return;
    }
    centered(pg, "Look at the camera", 34, TEXT, 22, true);
    centered(pg, p.name, 64, TEXT2, 15, true);
    constexpr int SIZE = 236, X = (PANEL - SIZE) / 2, Y = 98;
    lv_obj_t *ring = box(pg, X - 6, Y - 6, SIZE + 12, SIZE + 12, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(accent_of(p).glow), 0);
    if (!face_id_preview_create(pg, X, Y, SIZE)) {
        lv_obj_t *c = box(pg, X, Y, SIZE, SIZE, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(c, lv_color_hex(CARD), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        icon(c, "person", SIZE / 4, SIZE / 4, SIZE / 2, TEXT3, 3.0f);
    }
    // A tap on the preview tries again.
    lv_obj_t *tap = box(pg, X, Y, SIZE, SIZE, LV_RADIUS_CIRCLE);
    on(tap, [](lv_event_t *) {
        start_face();
    });
    U.status = centered(pg, "", 346, TEXT2, 15, true, 300);
    lv_obj_t *cancel = field_button(pg, (PANEL - 140) / 2, 384, 140, 44, "Cancel", 16, TEXT2);
    on(cancel, [](lv_event_t *) {
        go(U.back_to);
    });
    start_face();
}

// ---- the PIN pad ----------------------------------------------------------------------------
constexpr int KEY = 64, KEY_DX = 84, KEY_DY = 74, KEY_TOP = 146;  // the keys' size, pitch and first row
// Keys 0-9, then BACKSPACE and ENTER.
constexpr int K_BACK = 10, K_ENTER = 11;

void draw_dots()
{
    if (!U.dots) {
        return;
    }
    lv_obj_clean(U.dots);
    const int n = int(strlen(U.digits));
    const int shown = std::max(4, std::min(PROFILE_PIN_MAX, n + (n < PROFILE_PIN_MAX ? 1 : 0)));
    constexpr int D = 14, GAP = 14;
    const int x0 = (PANEL - (shown * D + (shown - 1) * GAP)) / 2;
    for (int i = 0; i < shown; ++i) {
        lv_obj_t *d = box(U.dots, x0 + i * (D + GAP), 0, D, D, LV_RADIUS_CIRCLE);
        if (i < n) {
            lv_obj_set_style_bg_color(d, lv_color_hex(TEXT), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_border_width(d, 2, 0);
            lv_obj_set_style_border_color(d, lv_color_hex(i < PROFILE_PIN_MIN ? TEXT3 : EDGE), 0);
        }
    }
}

void pin_status(const std::string &text, uint32_t color)
{
    if (U.status) {
        set_text(U.status, text.c_str());
        set_text_color(U.status, lv_color_hex(color));
    }
}

void shake()
{
    if (!U.dots) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, U.dots);
    // A decaying wobble around 0: three swings over the animation's 0-1000.
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
        const float t = float(v) / 1000.0f;
        lv_obj_set_x(static_cast<lv_obj_t *>(o), int32_t(lroundf(sinf(t * 6.2832f * 3.0f) * (1.0f - t) * 12.0f)));
    });
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 420);
    lv_anim_start(&a);
    if (U.hooks.haptic) {
        U.hooks.haptic(true);
    }
}

// The wait after too many tries, counted down; the keys answer again after it.
void wait_tick(lv_timer_t *)
{
    const uint32_t w = profiles_wait_s(U.target);
    if (!w) {
        stop_timer();
        pin_status("Enter the PIN", TEXT2);
        return;
    }
    char text[48];
    if (w >= 60) {
        snprintf(text, sizeof(text), "Too many tries. Wait %u min %u s", unsigned(w / 60), unsigned(w % 60));
    } else {
        snprintf(text, sizeof(text), "Too many tries. Wait %u s", unsigned(w));
    }
    pin_status(text, WARN);
}

bool waiting()
{
    return U.pin_for == PinFor::SignIn && profiles_wait_s(U.target) > 0;
}

void start_wait()
{
    wait_tick(nullptr);
    if (!U.timer && profiles_wait_s(U.target)) {
        U.timer = lv_timer_create(wait_tick, 1000, nullptr);
    }
}

struct Job {
    uint32_t generation;
    PinFor pin_for;
    uint8_t id;
    char pin[PROFILE_PIN_MAX + 1];
    std::string name;
    profile_result_t result = PROFILE_ERR_CRYPTO;
    profile_pin_t hashed{};
    bool hashed_ok = false;
};

void run_job(void (*work)(void *), void (*done)(void *), Job *job)
{
    if (U.hooks.background) {
        U.hooks.background(work, done, job);
    } else {
        work(job);
        done(job);
    }
}

void job_work(void *arg)  // off the LVGL task
{
    Job *j = static_cast<Job *>(arg);
    if (j->pin_for == PinFor::SignIn) {
        j->result = profiles_check_pin(j->id, j->pin);
    } else {
        j->hashed_ok = profiles_hash_pin(j->pin, &j->hashed);
    }
    wipe(j->pin, sizeof(j->pin));
}

void job_done(void *arg)  // the LVGL task
{
    Job *j = static_cast<Job *>(arg);
    const bool current = j->generation == U.generation && U.shown == Page::Pin;
    U.busy = false;
    if (j->pin_for == PinFor::SignIn) {
        if (current) {
            switch (j->result) {
            case PROFILE_OK: {
                const profile_result_t r = profiles_sign_in(j->id);
                if (r == PROFILE_OK) {
                    signed_in(j->id);
                } else if (r == PROFILE_ERR_NEED_FACE) {
                    go(Page::Face);  // the face's minute passed
                } else {
                    pin_status(profiles_result_text(r), RED);
                }
                break;
            }
            case PROFILE_ERR_WRONG_PIN: {
                shake();
                const int left = profiles_tries_left(j->id);
                char text[48];
                snprintf(text, sizeof(text), left == 1 ? "Wrong PIN \xC2\xB7 1 try left" : "Wrong PIN \xC2\xB7 %d tries left", left);
                pin_status(text, RED);
                break;
            }
            case PROFILE_ERR_WAIT:
                shake();
                start_wait();
                break;
            default:
                pin_status(profiles_result_text(j->result), RED);
                break;
            }
        }
    } else if (!j->hashed_ok) {
        profiles_ui_note(profiles_result_text(PROFILE_ERR_CRYPTO));
        if (current) {
            go(j->pin_for == PinFor::Setup ? Page::Intro : Page::Edit);
        }
    } else if (j->pin_for == PinFor::Setup) {
        uint8_t id = 0;
        const profile_result_t r = profiles_create_owner(j->name.c_str(), &j->hashed, &id);
        profiles_ui_note(r == PROFILE_OK ? "Profiles are on. You're signed in as the owner." : profiles_result_text(r));
        if (U.root) {
            go(r == PROFILE_OK ? Page::List : Page::Intro);
        }
    } else {
        U.saving = true;
        const profile_result_t r = profiles_set_pin(j->id, &j->hashed, PROFILE_BY_SESSION);
        U.saving = false;
        profiles_ui_note(r == PROFILE_OK ? "PIN saved" : profiles_result_text(r));
        if (U.root) {
            go(manage() ? Page::Edit : Page::List);
        }
    }
    wipe(reinterpret_cast<char *>(&j->hashed), sizeof(j->hashed));
    delete j;
    if (current) {
        wipe(U.digits, sizeof(U.digits));
        draw_dots();
    }
}

void submit()
{
    const size_t n = strlen(U.digits);
    if (U.busy || n < PROFILE_PIN_MIN) {
        if (!U.busy) {
            pin_status("4 to 6 digits", WARN);
        }
        return;
    }
    if (U.pin_for != PinFor::SignIn) {
        // A new PIN: typed twice.
        if (!U.first[0]) {
            memcpy(U.first, U.digits, sizeof(U.first));
            wipe(U.digits, sizeof(U.digits));
            draw_dots();
            pin_status("Once more to confirm", TEXT2);
            return;
        }
        const bool same = !strcmp(U.first, U.digits);
        wipe(U.first, sizeof(U.first));
        if (!same) {
            wipe(U.digits, sizeof(U.digits));
            draw_dots();
            shake();
            pin_status("They differ. Enter a new PIN again", RED);
            return;
        }
    }
    Job *j = new Job{};
    j->generation = U.generation;
    j->pin_for = U.pin_for;
    j->id = U.target;
    memcpy(j->pin, U.digits, sizeof(j->pin));
    j->name = "Owner";
    U.busy = true;
    pin_status(U.pin_for == PinFor::SignIn ? "Checking\xE2\x80\xA6" : "Saving\xE2\x80\xA6", TEXT2);
    run_job(job_work, job_done, j);
}

void key_cb(lv_event_t *e)
{
    const int k = int(data_of(e));
    if (U.busy || waiting()) {
        return;
    }
    size_t n = strlen(U.digits);
    if (k == K_ENTER) {
        submit();
        return;
    }
    if (k == K_BACK) {
        if (n) {
            U.digits[n - 1] = 0;
        }
    } else if (n < PROFILE_PIN_MAX) {
        U.digits[n] = char('0' + k);
        U.digits[n + 1] = 0;
    }
    draw_dots();
}

void pin_page()
{
    lv_obj_t *pg = frame(Page::Pin);
    wipe(U.digits, sizeof(U.digits));
    profile_t p{};
    profiles_get(U.target, &p);
    round_button(pg, (PANEL - 36) / 2, 14, 36, "x", [](lv_event_t *) {
        go(U.back_to);
    });
    const char *title = U.pin_for == PinFor::SignIn ? p.name : U.pin_for == PinFor::Setup ? "The owner's PIN" : "New PIN";
    centered(pg, title, 56, TEXT, 20, true, 300);
    U.dots = box(pg, 0, 92, PANEL, 16);
    draw_dots();
    U.status = centered(pg, "", 116, TEXT2, 14, true, 300);
    if (!U.message.empty()) {
        pin_status(U.message, U.message_color);
        U.message.clear();
    } else {
        pin_status(U.pin_for == PinFor::SignIn ? "Enter the PIN" : "4 to 6 digits", TEXT2);
    }
    // 1 2 3 / 4 5 6 / 7 8 9 / back 0 enter: every key's corner inside the circle.
    static const int ORDER[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, K_BACK, 0, K_ENTER};
    for (int i = 0; i < 12; ++i) {
        const int k = ORDER[i];
        const int cx = PANEL / 2 + (i % 3 - 1) * KEY_DX, cy = KEY_TOP + KEY / 2 + (i / 3) * KEY_DY;
        lv_obj_t *b = box(pg, cx - KEY / 2, cy - KEY / 2, KEY, KEY, LV_RADIUS_CIRCLE);
        if (k == K_ENTER) {
            style_fill(b, GREEN, false);
            icon(b, "check", 18, 18, 28, GREEN.ink, 2.6f);
        } else {
            lv_obj_set_style_bg_color(b, lv_color_hex(k == K_BACK ? CARD : FIELD), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(EDGE), LV_STATE_PRESSED);
            if (k == K_BACK) {
                icon(b, "back", 19, 19, 26, TEXT2, 2.2f);
            } else {
                const char text[2] = {char('0' + k), 0};
                const lv_font_t *f = font(26, true);
                label(b, text, 0, (KEY - lv_font_get_line_height(f)) / 2, KEY, TEXT, 26, true, LV_TEXT_ALIGN_CENTER);
            }
        }
        press_fx(b);
        on(b, key_cb, as_data(k));
    }
    if (waiting()) {
        start_wait();
    }
}

void choose(uint8_t id)
{
    profile_t p{};
    if (!profiles_get(id, &p)) {
        return;
    }
    if (id == profiles_current()) {
        close_later();
        return;
    }
    U.target = id;
    U.back_to = Page::Pick;
    const uint8_t signin = p.elevated ? uint8_t(PROFILE_SIGNIN_FACE_PIN) : p.signin;
    if (id == PROFILE_GUEST || signin == PROFILE_SIGNIN_NONE) {
        const profile_result_t r = profiles_sign_in(id);
        if (r == PROFILE_OK) {
            signed_in(id);
        } else {
            profiles_ui_note(profiles_result_text(r));
        }
        return;
    }
    if (signin == PROFILE_SIGNIN_FACE || signin == PROFILE_SIGNIN_FACE_PIN) {
        go(Page::Face);
    } else {
        U.pin_for = PinFor::SignIn;
        go(Page::Pin);
    }
}

// ---- Settings > Profiles ----------------------------------------------------------------------
void close_cb(lv_event_t *)
{
    close_later();
}

void intro_page()
{
    settings_frame(Page::Intro, "Profiles", close_cb);
    lv_obj_t *s = U.scroller;
    lv_obj_t *c = box(s, 190, 0, 100, 100, LV_RADIUS_CIRCLE);
    style_glass(c, VIOLET);
    icon(c, "person", 25, 25, 50, VIOLET.glow, 2.6f);
    centered(s, "Profiles are off", 114, TEXT, 24, true);
    wrapped(s, "Everyone sees every app, as now. Set up an owner with a PIN, then give Partner, Guest and others "
            "their own apps and permissions. Full mode adds your face.", 70, 150, 340, TEXT2, 15);
    lv_obj_t *b = fill_button(s, (PANEL - 240) / 2, 248, 240, 52, "Set up profiles", VIOLET, 17);
    on(b, [](lv_event_t *) {
        U.pin_for = PinFor::Setup;
        U.back_to = Page::Intro;
        wipe(U.first, sizeof(U.first));
        U.message = "Choose the owner's PIN: 4 to 6 digits";
        U.message_color = TEXT2;
        go(Page::Pin);
    });
    finish_scroller(310);
    stagger(U.page, 0, 14);
}

void edit_cb(lv_event_t *e)
{
    const uint8_t id = uint8_t(data_of(e));
    if (manage()) {
        U.target = id;
        U.keep_scroll = -1;
        go(Page::Edit);
    } else if (id == profiles_current() && id != PROFILE_GUEST) {
        // Not a manager: still your own PIN.
        U.target = id;
        U.pin_for = PinFor::Set;
        U.back_to = Page::List;
        wipe(U.first, sizeof(U.first));
        U.message = "Your new PIN: 4 to 6 digits";
        U.message_color = TEXT2;
        go(Page::Pin);
    } else {
        profiles_ui_note(profiles_any_elevated() ? "Only full mode changes profiles" : "This profile can't change profiles");
    }
}

// The capsules' choices for the lock times (seconds).
constexpr int LOCK_CHOICES[] = {0, 60, 120, 300, 600};
constexpr const char *LOCK_LABELS[] = {"Never", "1 min", "2 min", "5 min", "10 min"};
constexpr int FULL_CHOICES[] = {60, 120, 300};
constexpr const char *FULL_LABELS[] = {"1 min", "2 min", "5 min"};

int choice_index(const int *choices, int n, int value)
{
    int best = 0;
    for (int i = 0; i < n; ++i)
        if (abs(choices[i] - value) < abs(choices[best] - value)) {
            best = i;
        }
    return best;
}

void save_settings(const profiles_settings_t &s)
{
    U.saving = true;
    const profile_result_t r = profiles_set_settings(&s, PROFILE_BY_SESSION);
    U.saving = false;
    if (r != PROFILE_OK) {
        profiles_ui_note(profiles_result_text(r));
        rebuild_keeping_scroll();
    }
}

void list_page()
{
    const bool restoring = U.keep_scroll > 0;
    settings_frame(Page::List, "Profiles", close_cb);
    lv_obj_t *s = U.scroller;
    const bool can = manage();
    U.off_armed = false;
    int y = 4;
    if (!can) {
        lv_obj_t *n = glass(s, COLUMN_X, y, COLUMN_W, 64, 22, AMBER);
        wrapped(n, profiles_any_elevated() ? "Sign in with full mode to change profiles." : "This profile can't change profiles.",
                16, 12, COLUMN_W - 32, TEXT2, 14);
        y += 76;
    }
    profile_t all[SLOTS];
    const int count = profiles_list(all, SLOTS);
    std::vector<profile_t> list;
    for (int i = 0; i < count; ++i)
        if (all[i].id != PROFILE_GUEST) {
            list.push_back(all[i]);
        }
    for (int i = 0; i < count; ++i)
        if (all[i].id == PROFILE_GUEST) {
            list.push_back(all[i]);
        }
    for (const profile_t &p : list) {
        lv_obj_t *row = glass(s, COLUMN_X, y, COLUMN_W, 72, 24, accent_of(p));
        avatar(row, 14, 12, 48, p, p.id == profiles_current());
        lv_obj_t *name = label(row, p.name, 76, 13, 200, TEXT, 18, true);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        char detail[64];
        snprintf(detail, sizeof(detail), "%s%s", signin_words(p), p.id == profiles_current() ? " \xC2\xB7 now" : "");
        label(row, detail, 76, 40, 250, TEXT3, 13);
        icon(row, "chev", COLUMN_W - 40, 24, 22, TEXT3, 1.8f);
        press_fx(row);
        on(row, edit_cb, as_data(p.id));
        y += 82;
    }
    if (can && count < SLOTS) {
        lv_obj_t *add = fill_button(s, (PANEL - 220) / 2, y + 4, 220, 48, "Add profile", VIOLET, 16);
        on(add, [](lv_event_t *) {
            profile_t p{};
            profiles_defaults(PROFILE_NEW, &p);
            p.id = PROFILE_NEW;
            // A name not taken yet: "Profile 2", "Profile 3"...
            profile_t all_now[SLOTS];
            const int n = profiles_list(all_now, SLOTS);
            for (int k = 2; k < 20; ++k) {
                snprintf(p.name, sizeof(p.name), "Profile %d", k);
                bool taken = false;
                for (int i = 0; i < n; ++i) {
                    taken |= !strcmp(all_now[i].name, p.name);
                }
                if (!taken) {
                    break;
                }
            }
            U.saving = true;
            const profile_result_t r = profiles_save(&p, PROFILE_BY_SESSION);
            U.saving = false;
            if (r != PROFILE_OK) {
                profiles_ui_note(profiles_result_text(r));
                return;
            }
            U.target = p.id;
            U.keep_scroll = -1;
            go(Page::Edit);
        });
        y += 64;
    }
    profiles_settings_t st{};
    profiles_get_settings(&st);
    // Auto-lock.
    y += 10;
    section(s, "LOCKING", y);
    y += 24;
    lv_obj_t *lc = glass(s, COLUMN_X, y, COLUMN_W, 186, 26, VIOLET);
    label(lc, "Lock after", 20, 14, 200, TEXT, 16, true);
    lv_obj_t *cap = capsule(lc, 12, 40, COLUMN_W - 24, 36, LOCK_LABELS, 5, choice_index(LOCK_CHOICES, 5, st.lock_s), VIOLET, 13);
    lv_obj_add_event_cb(cap, [](lv_event_t *e) {
        profiles_settings_t v{};
        profiles_get_settings(&v);
        v.lock_s = uint16_t(LOCK_CHOICES[capsule_selected(lv_event_get_current_target_obj(e))]);
        save_settings(v);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    label(lc, "Full mode locks after", 20, 92, 250, TEXT, 16, true);
    lv_obj_t *fc = capsule(lc, 12, 118, COLUMN_W - 24, 36, FULL_LABELS, 3, choice_index(FULL_CHOICES, 3, st.elevated_lock_s),
                           AMBER, 13);
    lv_obj_add_event_cb(fc, [](lv_event_t *e) {
        profiles_settings_t v{};
        profiles_get_settings(&v);
        v.elevated_lock_s = uint16_t(FULL_CHOICES[capsule_selected(lv_event_get_current_target_obj(e))]);
        save_settings(v);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    label(lc, "unused, back to the profile below", 20, 160, 300, TEXT3, 13);
    if (!can) {
        lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(fc, LV_OBJ_FLAG_CLICKABLE);
    }
    y += 198;
    // What the board locks to: Guest, or a profile without sign-in.
    lv_obj_t *dc = glass(s, COLUMN_X, y, COLUMN_W, 64, 24, VIOLET);
    label(dc, "When locked", 20, 21, 160, TEXT, 16, true);
    profile_t d{};
    profiles_get(st.default_id, &d);
    label(dc, d.name, 150, 21, COLUMN_W - 200, TEXT2, 16, true, LV_TEXT_ALIGN_RIGHT);
    icon(dc, "chev", COLUMN_W - 40, 20, 22, TEXT3, 1.8f);
    if (can) {
        press_fx(dc);
        on(dc, [](lv_event_t *) {
            profiles_settings_t v{};
            profiles_get_settings(&v);
            profile_t ps[SLOTS];
            const int n = profiles_list(ps, SLOTS);
            int at = 0;
            for (int i = 0; i < n; ++i)
                if (ps[i].id == v.default_id) {
                    at = i;
                }
            for (int k = 1; k <= n; ++k) {
                const profile_t &c = ps[(at + k) % n];
                if (c.signin == PROFILE_SIGNIN_NONE && !c.elevated) {
                    v.default_id = c.id;
                    break;
                }
            }
            save_settings(v);
            rebuild_keeping_scroll();
        });
    }
    y += 76;
    if (can) {
        lv_obj_t *off = field_button(s, (PANEL - 240) / 2, y + 6, 240, 46, "Turn off profiles", 15, RED);
        on(off, [](lv_event_t *e) {
            lv_obj_t *b = lv_event_get_current_target_obj(e);
            if (!U.off_armed) {
                U.off_armed = true;
                set_text(lv_obj_get_child(b, 0), "Tap again: all profiles go");
                return;
            }
            U.saving = true;
            const profile_result_t r = profiles_turn_off(PROFILE_BY_SESSION);
            U.saving = false;
            profiles_ui_note(r == PROFILE_OK ? "Profiles are off: every app for everyone" : profiles_result_text(r));
            go(r == PROFILE_OK ? Page::Intro : Page::List);
        });
        y += 64;
    }
    wrapped(s, "Forgot a PIN? With the board on USB, RPC 61 on a computer sets a new one (see CONTRIBUTING).",
            COLUMN_X + 10, y + 8, COLUMN_W - 20, TEXT3, 13);
    finish_scroller(y + 60);
    if (!restoring) {
        stagger(U.scroller, 0, 12);
    }
}

// ---- the editor -------------------------------------------------------------------------------
bool load_target(profile_t &p)
{
    if (!profiles_get(U.target, &p)) {
        go(Page::List);
        return false;
    }
    return true;
}

// Saves a change; a refusal says why and draws the stored profile again.
bool save_target(profile_t &p, bool rebuild)
{
    U.saving = true;
    const profile_result_t r = profiles_save(&p, PROFILE_BY_SESSION);
    U.saving = false;
    if (r != PROFILE_OK) {
        profiles_ui_note(profiles_result_text(r));
        rebuild_keeping_scroll();
        return false;
    }
    if (rebuild) {
        rebuild_keeping_scroll();
    }
    return true;
}

const char *const ACCESS3[] = {"Hidden", "Read", "Full"};
const char *const ACCESS2[] = {"Hidden", "Shown"};

void app_cb(lv_event_t *e)
{
    profile_t p{};
    if (!load_target(p)) {
        return;
    }
    const int i = int(data_of(e));
    const ProfilesUiApp &app = U.hooks.apps[i];
    const int sel = capsule_selected(lv_event_get_current_target_obj(e));
    p.apps[app.slot] = uint8_t(app.read_only ? sel : sel ? PROFILE_ACCESS_FULL : PROFILE_ACCESS_HIDDEN);
    save_target(p, false);
}

void preset_cb(lv_event_t *e)
{
    profile_t p{};
    if (!load_target(p)) {
        return;
    }
    const int preset = int(data_of(e));  // 0 all, 1 read-only, 2 none
    for (int i = 0; i < U.hooks.app_count; ++i) {
        const ProfilesUiApp &app = U.hooks.apps[i];
        p.apps[app.slot] = uint8_t(preset == 0 ? PROFILE_ACCESS_FULL : preset == 2 ? PROFILE_ACCESS_HIDDEN
                                   : app.read_only ? PROFILE_ACCESS_READ : PROFILE_ACCESS_FULL);
    }
    save_target(p, true);
}

void cap_cb(lv_event_t *e)
{
    profile_t p{};
    if (!load_target(p)) {
        return;
    }
    const uint32_t bit = PROFILE_CAP(int(data_of(e)));
    p.caps = toggle_on(lv_event_get_current_target_obj(e)) ? p.caps | bit : p.caps & ~bit;
    save_target(p, false);
}

// Enrolment ended (face_id calls this once, on the LVGL task; at once, from
// face_id_cancel, when the page goes).
void enroll_done(face_id_result_t result, void *user)
{
    U.face_running = false;
    if (result == FACE_ID_CANCELLED || reinterpret_cast<uintptr_t>(user) != U.generation) {
        return;
    }
    if (result == FACE_ID_OK) {
        profile_t q{};
        if (U.full_after_face && load_target(q)) {
            q.elevated = true;
            q.signin = PROFILE_SIGNIN_FACE_PIN;
            U.saving = true;
            const profile_result_t r = profiles_save(&q, PROFILE_BY_SESSION);
            U.saving = false;
            profiles_ui_note(r == PROFILE_OK ? "Face added. Full mode is on" : profiles_result_text(r));
        } else {
            profiles_ui_note("Face added");
        }
        U.full_after_face = false;
        go(Page::Edit);
        return;
    }
    face_status(result == FACE_ID_NO_FACE ? "No face seen. Tap to try again"
                : result == FACE_ID_TIMEOUT ? "Not long enough. Tap to try again"
                : result == FACE_ID_MANY_FACES ? "One face at a time. Tap to try again"
                : result == FACE_ID_NO_CAMERA ? "Face sign-in needs the camera module"
                : result == FACE_ID_BUSY ? "The camera is busy. Tap to try again" : "Could not add the face", WARN);
}

void start_enroll()
{
    if (U.face_running) {
        return;
    }
    U.face_running = face_id_enroll_start(U.target, enroll_done, reinterpret_cast<void *>(uintptr_t(U.generation)));
    face_status(U.face_running ? "Look at the camera and turn your head slowly" : "The camera is busy. Tap to try again",
                U.face_running ? TEXT2 : WARN);
}

// Adding a face: the live picture, and face ID's own hints in it.
void enroll_page()
{
    lv_obj_t *pg = frame(Page::Enroll);
    profile_t p{};
    profiles_get(U.target, &p);
    centered(pg, "Add a face", 34, TEXT, 22, true);
    centered(pg, p.name, 64, TEXT2, 15, true);
    constexpr int SIZE = 236, X = (PANEL - SIZE) / 2, Y = 98;
    if (!face_id_preview_create(pg, X, Y, SIZE)) {
        lv_obj_t *c = box(pg, X, Y, SIZE, SIZE, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(c, lv_color_hex(CARD), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        icon(c, "person", SIZE / 4, SIZE / 4, SIZE / 2, TEXT3, 3.0f);
    }
    lv_obj_t *tap = box(pg, X, Y, SIZE, SIZE, LV_RADIUS_CIRCLE);
    on(tap, [](lv_event_t *) {
        start_enroll();
    });
    U.status = centered(pg, "", 346, TEXT2, 15, true, 320);
    lv_obj_t *cancel = field_button(pg, (PANEL - 140) / 2, 384, 140, 44, "Cancel", 16, TEXT2);
    on(cancel, [](lv_event_t *) {
        U.full_after_face = false;
        go(Page::Edit);
    });
    start_enroll();
}

void edit_page()
{
    profile_t p{};
    if (!load_target(p)) {
        return;
    }
    settings_frame(Page::Edit, p.name, [](lv_event_t *) {
        stop_face();
        go(Page::List);
    });
    lv_obj_t *s = U.scroller;
    U.delete_armed = false;
    const bool guest = p.id == PROFILE_GUEST;
    const bool face = face_id_available();
    const bool enrolled = profiles_face_enrolled(p.id);
    int y = 4;
    // Who: avatar, name, colour.
    lv_obj_t *who = glass(s, COLUMN_X, y, COLUMN_W, 136, 26, accent_of(p));
    avatar(who, 18, 16, 60, p);
    lv_obj_t *nm = label(who, p.name, 92, 20, 170, TEXT, 20, true);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    label(who, signin_words(p), 92, 48, 250, TEXT3, 13);
    lv_obj_t *rename = field_button(who, COLUMN_W - 104, 22, 88, 36, "Rename", 14, TEXT2);
    on(rename, [](lv_event_t *) {
        go(Page::Rename);
    });
    for (int c = 0; c < PROFILE_COLORS; ++c) {
        lv_obj_t *sw = box(who, 20 + c * 38, 92, 28, 28, LV_RADIUS_CIRCLE);
        style_fill(sw, *PALETTE[c], false);
        if (c == p.color) {
            lv_obj_set_style_border_width(sw, 3, 0);
            lv_obj_set_style_border_color(sw, lv_color_hex(TEXT), 0);
        }
        lv_obj_set_ext_click_area(sw, 6);
        on(sw, [](lv_event_t *e) {
            profile_t q{};
            if (load_target(q)) {
                q.color = uint8_t(data_of(e));
                save_target(q, true);
            }
        }, as_data(c));
    }
    y += 148;
    if (!guest) {
        // Sign-in: how, the PIN and the face.
        section(s, "SIGN-IN", y);
        y += 24;
        lv_obj_t *si = glass(s, COLUMN_X, y, COLUMN_W, 150, 26, VIOLET);
        static const char *const METHODS[] = {"None", "PIN", "Face", "Both"};
        lv_obj_t *m = capsule(si, 12, 14, COLUMN_W - 24, 38, METHODS, 4, p.signin, VIOLET, 14);
        lv_obj_add_event_cb(m, [](lv_event_t *e) {
            profile_t q{};
            if (load_target(q)) {
                q.signin = uint8_t(capsule_selected(lv_event_get_current_target_obj(e)));
                save_target(q, true);
            }
        }, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_t *pin = field_button(si, 12, 66, (COLUMN_W - 36) / 2, 40, p.has_pin ? "Change PIN" : "Set PIN", 15);
        on(pin, [](lv_event_t *) {
            U.pin_for = PinFor::Set;
            U.back_to = Page::Edit;
            wipe(U.first, sizeof(U.first));
            U.message = "4 to 6 digits";
            U.message_color = TEXT2;
            go(Page::Pin);
        });
        lv_obj_t *fb = field_button(si, 24 + (COLUMN_W - 36) / 2, 66, (COLUMN_W - 36) / 2, 40,
                                    !face ? "No camera" : enrolled ? "Redo face" : "Add face", 15, face ? TEXT : TEXT3);
        if (face) {
            on(fb, [](lv_event_t *) {
                go(Page::Enroll);
            });
        }
        char state[80];
        snprintf(state, sizeof(state), "%s \xC2\xB7 %s", p.has_pin ? "PIN set" : "No PIN",
                 !face ? "face needs the camera module" : enrolled ? "face added" : "no face");
        label(si, state, 20, 118, COLUMN_W - 40, TEXT3, 13);
        y += 162;
        // Full mode: face and PIN; the dangerous permissions act only then.
        lv_obj_t *fm = glass(s, COLUMN_X, y, COLUMN_W, 104, 26, AMBER);
        label(fm, "Full mode", 20, 16, 200, TEXT, 18, true);
        wrapped(fm, "Signs in with face and PIN. Developer mode, machines, Paseo and the collector need it.", 20, 44,
                COLUMN_W - 120, TEXT3, 13, LV_TEXT_ALIGN_LEFT);
        lv_obj_t *t = toggle(fm, COLUMN_W - 76, 18, p.elevated, AMBER);
        lv_obj_add_event_cb(t, [](lv_event_t *e) {
            profile_t q{};
            if (!load_target(q)) {
                return;
            }
            q.elevated = toggle_on(lv_event_get_current_target_obj(e));
            if (q.elevated && !profiles_face_enrolled(q.id)) {
                // Full mode signs in with face and PIN: without a face the save
                // would only be refused, so add the face first and full mode
                // turns on once it is in (enroll_done).
                if (!q.has_pin) {
                    profiles_ui_note("Set a PIN first, then add your face");
                } else if (!face_id_available()) {
                    profiles_ui_note("Full mode needs the camera module for your face");
                } else {
                    U.full_after_face = true;
                    go(Page::Enroll);
                    return;
                }
                rebuild_keeping_scroll();  // the switch goes back off
                return;
            }
            if (q.elevated) {
                q.signin = PROFILE_SIGNIN_FACE_PIN;
            }
            const bool self = q.id == profiles_current();
            if (save_target(q, true) && q.elevated && self && !profiles_elevated()) {
                profiles_ui_note("Sign in with face and PIN to use full mode");
            }
        }, LV_EVENT_VALUE_CHANGED, nullptr);
        y += 116;
    }
    // Apps: hidden, read-only (where an app has it) or full.
    section(s, "APPS", y);
    y += 24;
    static const char *const PRESETS[] = {"All", "Read-only", "None"};
    for (int i = 0; i < 3; ++i) {
        const int bw = (COLUMN_W - 24) / 3;
        lv_obj_t *b = field_button(s, COLUMN_X + i * (bw + 12), y, bw, 36, PRESETS[i], 14, TEXT2);
        on(b, preset_cb, as_data(i));
    }
    y += 46;
    lv_obj_t *apps = glass(s, COLUMN_X, y, COLUMN_W, U.hooks.app_count * 54 + 12, 26, VIOLET);
    for (int i = 0; i < U.hooks.app_count; ++i) {
        const ProfilesUiApp &app = U.hooks.apps[i];
        const int ry = 6 + i * 54;
        if (app.glyph) {
            icon(apps, app.glyph, 16, ry + 13, 24, TEXT2, 1.9f);
        } else {
            label(apps, "G", 16, ry + 13, 24, TEXT2, 18, true, LV_TEXT_ALIGN_CENTER);
        }
        lv_obj_t *nl = label(apps, app.name, 50, ry + 15, 110, TEXT, 15, true);
        lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);
        const uint8_t access = p.apps[app.slot];
        const int sel = app.read_only ? access : access == PROFILE_ACCESS_HIDDEN ? 0 : 1;
        lv_obj_t *c = capsule(apps, 160, ry + 8, COLUMN_W - 172, 36, app.read_only ? ACCESS3 : ACCESS2,
                              app.read_only ? 3 : 2, sel, VIOLET, 13);
        lv_obj_add_event_cb(c, app_cb, LV_EVENT_VALUE_CHANGED, as_data(i));
    }
    y += U.hooks.app_count * 54 + 24;
    // Permissions.
    section(s, "PERMISSIONS", y);
    y += 24;
    std::vector<int> caps;
    for (int c = 0; c < PROFILE_CAP_COUNT; ++c) {
        const uint32_t bit = PROFILE_CAP(c);
        if (guest && (bit & (PROFILE_CAPS_ELEVATED | PROFILE_CAP(PROFILE_CAP_SETTINGS_PROFILES)))) {
            continue;  // never Guest's
        }
        caps.push_back(c);
    }
    lv_obj_t *pc = glass(s, COLUMN_X, y, COLUMN_W, int(caps.size()) * 54 + 12, 26, VIOLET);
    for (size_t i = 0; i < caps.size(); ++i) {
        const int c = caps[i];
        const bool dangerous = PROFILE_CAP(c) & PROFILE_CAPS_ELEVATED;
        const int ry = 6 + int(i) * 54;
        lv_obj_t *t = label(pc, profiles_cap_title(profile_cap_t(c)), 20, ry + (dangerous ? 8 : 16), 230,
                            dangerous && !p.elevated ? TEXT3 : TEXT, 15, true);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        if (dangerous) {
            label(pc, p.elevated ? "full mode" : "only in full mode", 20, ry + 30, 230, p.elevated ? AMBER.glow : TEXT3, 12);
        }
        lv_obj_t *tg = toggle(pc, COLUMN_W - 76, ry + 11, p.caps & PROFILE_CAP(c), dangerous ? AMBER : VIOLET);
        lv_obj_add_event_cb(tg, cap_cb, LV_EVENT_VALUE_CHANGED, as_data(c));
    }
    y += int(caps.size()) * 54 + 24;
    if (!guest) {
        lv_obj_t *del = field_button(s, (PANEL - 220) / 2, y, 220, 46, "Delete profile", 15, RED);
        on(del, [](lv_event_t *e) {
            if (!U.delete_armed) {
                U.delete_armed = true;
                set_text(lv_obj_get_child(lv_event_get_current_target_obj(e), 0), "Tap again to delete");
                return;
            }
            U.saving = true;
            const profile_result_t r = profiles_delete(U.target, PROFILE_BY_SESSION);
            U.saving = false;
            profiles_ui_note(r == PROFILE_OK ? "Profile deleted" : profiles_result_text(r));
            if (r == PROFILE_OK) {
                U.keep_scroll = -1;
                go(manage() ? Page::List : Page::None);
            }
        });
        y += 58;
    }
    finish_scroller(y);
}

void rename_page()
{
    profile_t p{};
    if (!load_target(p)) {
        return;
    }
    lv_obj_t *pg = frame(Page::Rename);
    centered(pg, "Name", 90, TEXT, 22, true);
    U.area = lv_textarea_create(pg);
    lv_obj_remove_style_all(U.area);
    lv_obj_set_pos(U.area, 70, 136);
    lv_obj_set_size(U.area, 340, 52);
    lv_obj_set_style_bg_color(U.area, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(U.area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.area, 26, 0);
    lv_obj_set_style_pad_hor(U.area, 20, 0);
    lv_obj_set_style_pad_ver(U.area, 13, 0);
    lv_obj_set_style_text_font(U.area, font(20, true), 0);
    lv_obj_set_style_text_color(U.area, lv_color_hex(TEXT), 0);
    lv_textarea_set_one_line(U.area, true);
    lv_textarea_set_max_length(U.area, PROFILE_NAME_LEN - 1);
    lv_textarea_set_text(U.area, p.name);
    KeyboardConfig kc;
    kc.action = "Save";
    kc.accent = &VIOLET;
    keyboard(pg, U.area, 0, 236, PANEL, 234, kc);
    lv_obj_add_event_cb(U.area, [](lv_event_t *) {
        profile_t q{};
        if (!load_target(q)) {
            return;
        }
        strlcpy(q.name, lv_textarea_get_text(U.area), sizeof(q.name));
        U.saving = true;
        const profile_result_t r = profiles_save(&q, PROFILE_BY_SESSION);
        U.saving = false;
        if (r != PROFILE_OK) {
            profiles_ui_note(r == PROFILE_ERR_INVALID ? "A name of 1 to 19 letters" : profiles_result_text(r));
            return;
        }
        go(Page::Edit);
    }, LV_EVENT_READY, nullptr);
}

void show(Page page)
{
    if (page == Page::None) {
        profiles_ui_close();
        return;
    }
    ensure_root();
    switch (page) {
    case Page::Pick: pick_page(); break;
    case Page::Face: face_page(); break;
    case Page::Pin: pin_page(); break;
    case Page::Intro: intro_page(); break;
    case Page::List: list_page(); break;
    case Page::Edit: edit_page(); break;
    case Page::Rename: rename_page(); break;
    case Page::Enroll: enroll_page(); break;
    case Page::None: break;
    }
}

// ---- badges ------------------------------------------------------------------------------
// The badge's kind rides in its user data: 1 for Settings' entry.
bool settings_badge(lv_obj_t *b)
{
    return lv_obj_get_user_data(b) != nullptr;
}

void draw_badge(lv_obj_t *b)
{
    lv_obj_clean(b);
    const bool on = profiles_configured();
    set_hidden(b, !on && !settings_badge(b));
    if (!on) {
        if (settings_badge(b)) {  // profiles off: a person, the way to set them up
            const int size = int(lv_obj_get_style_width(b, LV_PART_MAIN));
            lv_obj_t *c = box(b, 0, 0, size, size, LV_RADIUS_CIRCLE);
            lv_obj_set_style_bg_color(c, lv_color_hex(FIELD), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
            icon(c, "person", size / 4, size / 4, size / 2, TEXT, 2.0f);
        }
        return;
    }
    profile_t p{};
    profiles_get(profiles_current(), &p);
    const int size = int(lv_obj_get_style_width(b, LV_PART_MAIN));
    lv_obj_t *a = avatar(b, 0, 0, size, p, false);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    if (profiles_elevated()) {  // full mode: an amber ring
        lv_obj_set_style_border_width(a, 2, 0);
        lv_obj_set_style_border_color(a, lv_color_hex(AMBER.glow), 0);
    }
}

// After the change, not inside it: the change may come from a tap on the page
// this rebuilds (Lock, a toggle).
bool refresh_pending = false, refresh_page = false;
void changed(void *)
{
    refresh_page |= !U.saving;  // our own change: the page drew it already
    if (refresh_pending) {
        return;
    }
    refresh_pending = true;
    lv_async_call([](void *) {
        refresh_pending = false;
        const bool page = refresh_page;
        refresh_page = false;
        for (lv_obj_t *b : U.badges) {
            draw_badge(b);
        }
        if (page && U.root) {
            profiles_ui_refresh();
        }
    }, nullptr);
}
}  // namespace

void profiles_ui_init(const ProfilesUiHooks &hooks)
{
    U.hooks = hooks;
    profiles_on_change(changed, nullptr);
}

void profiles_ui_switcher()
{
    if (!profiles_configured()) {
        profiles_ui_settings();
        return;
    }
    ensure_root();
    show(Page::Pick);
}

void profiles_ui_sign_in(uint8_t id)
{
    ensure_root();
    choose(id);
}

void profiles_ui_settings()
{
    ensure_root();
    U.keep_scroll = -1;
    show(profiles_configured() ? Page::List : Page::Intro);
}

void profiles_ui_close()
{
    stop_face();
    stop_timer();
    wipe(U.digits, sizeof(U.digits));
    wipe(U.first, sizeof(U.first));
    if (U.root) {
        lv_obj_delete(U.root);
    }
    U.root = U.page = U.scroller = U.dots = U.status = U.area = nullptr;
    U.shown = U.go = Page::None;
    U.busy = false;
    ++U.generation;
}

bool profiles_ui_showing()
{
    return U.root != nullptr;
}

bool profiles_ui_back()
{
    if (!U.root) {
        return false;
    }
    switch (U.shown) {
    case Page::Face:
    case Page::Pin:
        show(U.back_to);
        break;
    case Page::Edit:
    case Page::Rename:
    case Page::Enroll:
        show(U.shown == Page::Edit ? Page::List : Page::Edit);
        break;
    default:
        profiles_ui_close();
        break;
    }
    return true;
}

lv_obj_t *profiles_ui_badge(lv_obj_t *parent, int x, int y, int size, bool settings)
{
    lv_obj_t *b = box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_user_data(b, settings ? as_data(1) : nullptr);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 10);
    press_fx(b);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
        if (settings_badge(lv_event_get_current_target_obj(e))) {
            profiles_ui_settings();
        } else {
            profiles_ui_switcher();
        }
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
        lv_obj_t *o = lv_event_get_current_target_obj(e);
        U.badges.erase(std::remove(U.badges.begin(), U.badges.end(), o), U.badges.end());
    }, LV_EVENT_DELETE, nullptr);
    U.badges.push_back(b);
    draw_badge(b);
    return b;
}

void profiles_ui_note(const char *text)
{
    close_note();
    constexpr int W = 290, H = 46, Y = 378;
    lv_obj_t *pill = box(lv_layer_top(), (PANEL - W) / 2, Y, W, H, H / 2);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x202024), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(EDGE), 0);
    lv_obj_t *t = label(pill, text, 14, (H - lv_font_get_line_height(font(15, true))) / 2, W - 28, TEXT, 15, true,
                        LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    U.note = pill;
    U.note_timer = lv_timer_create([](lv_timer_t *) {
        U.note_timer = nullptr;  // a one-shot timer deletes itself
        if (U.note) {
            lv_obj_delete(U.note);
            U.note = nullptr;
        }
    }, 2600, nullptr);
    lv_timer_set_repeat_count(U.note_timer, 1);
}

lv_obj_t *profiles_ui_ribbon(lv_obj_t *parent, const char *text)
{
    // Low in the circle, narrow enough for its chord there; taps pass through.
    constexpr int H = 26, Y = PANEL - 48;
    const int w = std::min(text_width(text, 13, true) + 28, 2 * half_chord(Y + H) - 16);
    lv_obj_t *r = box(parent, (PANEL - w) / 2, Y, w, H, H / 2);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x2A2414), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_90, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(WARN), 0);
    lv_obj_set_style_border_opa(r, LV_OPA_50, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = label(r, text, 6, (H - lv_font_get_line_height(font(13, true))) / 2 - 1, w - 12, WARN, 13, true,
                        LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    return r;
}

void profiles_ui_refresh()
{
    for (lv_obj_t *b : U.badges) {
        draw_badge(b);
    }
    if (!U.root) {
        return;
    }
    switch (U.shown) {
    case Page::Pick:
        show(Page::Pick);
        break;
    case Page::List:
    case Page::Intro:
        U.keep_scroll = U.scroller ? lv_obj_get_scroll_y(U.scroller) : -1;
        show(profiles_configured() ? Page::List : Page::Intro);
        break;
    case Page::Edit:
    case Page::Rename:
    case Page::Enroll:
        // Locked meanwhile (or no longer a manager): the editor closes.
        if (!manage()) {
            profiles_ui_close();
        } else if (U.shown == Page::Edit) {
            rebuild_keeping_scroll();
        }
        break;
    case Page::Pin:
        if (U.pin_for == PinFor::Set && !manage() && U.target != profiles_current()) {
            profiles_ui_close();
        }
        break;
    default:
        break;
    }
}
