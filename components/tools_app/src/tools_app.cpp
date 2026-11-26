// SPDX-License-Identifier: Apache-2.0
// Tools: torch, level, compass, calculator and the Interaction board, on one
// screen with a tab capsule.
#include "tools_app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "interact.h"
#include "mui.h"
#include "nvs.h"

namespace {
using namespace mui;
const char *TAG = "tools";
// Tools' own colour: a warm coral.
const Accent CORAL = {0xFFB199, 0xE0533D, 0xFF8A70, 0x2A0A04, 0x3A1610, 0x1A0906};
const Accent &ACC = CORAL;
enum Tool { TORCH, LEVEL, COMPASS, CALC, BOARD, WEBCAM, TOOLS };
const char *const NAMES[TOOLS] = {"Torch", "Level", "Compass", "Calc", "Interact", "Webcam"};

struct {
    tools_hooks_t hooks{};
    lv_obj_t *screen = nullptr, *tabs = nullptr, *pane[TOOLS] = {};
    int tool = TORCH;
    bool built = false, active = false;
    lv_timer_t *timer = nullptr;
    // torch
    lv_obj_t *torch_btn = nullptr, *torch_label = nullptr, *torch_note = nullptr, *light = nullptr;
    bool torch_on = false, screen_light = false;
    // level
    lv_obj_t *dial = nullptr, *bubble = nullptr, *level_big = nullptr, *level_note = nullptr, *horizon = nullptr;
    lv_point_precise_t horizon_pts[2];
    bool was_level = false;
    float lx = 0, ly = 0;
    // compass
    lv_obj_t *rose = nullptr, *heading = nullptr, *cardinal = nullptr, *compass_note = nullptr, *letters[4] = {};
    bool mag = false, mag_tried = false;
    float mmin[3] = {1e9f, 1e9f, 1e9f}, mmax[3] = {-1e9f, -1e9f, -1e9f}, head = 0;
    // webcam
    lv_obj_t *cam_btn = nullptr, *cam_label = nullptr, *cam_note = nullptr, *cam_torch = nullptr;
    int cam_tick = 0;
    // calculator
    lv_obj_t *display = nullptr, *expr = nullptr;
    double acc = 0;
    char op = 0;
    std::string entry = "0";
    bool fresh = true;
} T;

lv_obj_t *round_button(lv_obj_t *parent, int x, int y, int size, uint32_t bg)
{
    lv_obj_t *b = box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    return b;
}

// ---- torch -----------------------------------------------------------------------------
void torch_look()
{
    lv_obj_set_style_bg_color(T.torch_btn, lv_color_hex(T.torch_on ? 0xFFF4D6 : FIELD), 0);
    lv_obj_set_style_line_color(lv_obj_get_child(T.torch_btn, 0), lv_color_hex(T.torch_on ? 0x3A2A00 : TEXT), 0);
    lv_label_set_text(T.torch_label, T.torch_on ? "On" : "Off");
    if (T.cam_torch) {  // the webcam page's own torch button
        lv_obj_set_style_bg_color(T.cam_torch, lv_color_hex(T.torch_on ? 0xFFF4D6 : FIELD), 0);
        lv_obj_set_style_line_color(lv_obj_get_child(T.cam_torch, 0), lv_color_hex(T.torch_on ? 0x3A2A00 : TEXT), 0);
    }
}

void torch_cb(lv_event_t *)
{
    const bool want = !T.torch_on;
    const bool ok = T.hooks.torch && T.hooks.torch(want);
    if (!ok && want) {
        lv_label_set_text(T.torch_note, "No camera module flash: use the screen light.");
        return;
    }
    T.torch_on = want;
    if (T.hooks.haptic) {
        T.hooks.haptic();
    }
    lv_label_set_text(T.torch_note, "The camera module's flash");
    torch_look();
}

void light_off_cb(lv_event_t *)
{
    T.screen_light = false;
    if (T.light) {
        lv_obj_add_flag(T.light, LV_OBJ_FLAG_HIDDEN);
    }
    if (T.hooks.brightness) {
        T.hooks.brightness(-1);
    }
}

void light_cb(lv_event_t *)
{
    T.screen_light = true;
    lv_obj_remove_flag(T.light, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(T.light);
    if (T.hooks.brightness) {
        T.hooks.brightness(100);
    }
}

void build_torch(lv_obj_t *p)
{
    T.torch_btn = round_button(p, 140, 20, 200, FIELD);
    icon(T.torch_btn, "torch", 60, 50, 80, TEXT, 1.7f);
    lv_obj_add_event_cb(T.torch_btn, torch_cb, LV_EVENT_CLICKED, nullptr);
    T.torch_label = label(p, "Off", 0, 232, 480, TEXT, 26, true, LV_TEXT_ALIGN_CENTER);
    T.torch_note = label(p, "The camera module's flash", 40, 270, 400, TEXT3, 15, false, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *b = field_button(p, 130, 300, 220, 46, "Screen light", 16, TEXT);
    lv_obj_add_event_cb(b, light_cb, LV_EVENT_CLICKED, nullptr);
    // The screen light: white over everything, tap to leave.
    T.light = box(T.screen, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(T.light, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(T.light, LV_OPA_COVER, 0);
    lv_obj_add_flag(T.light, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    label(T.light, "Tap to turn off", 0, 400, 480, 0x909090, 15, false, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_event_cb(T.light, light_off_cb, LV_EVENT_CLICKED, nullptr);
}

// ---- level -----------------------------------------------------------------------------
constexpr int DIAL = 236, BUBBLE = 44;

void build_level(lv_obj_t *p)
{
    T.dial = box(p, 240 - DIAL / 2, 6, DIAL, DIAL, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(T.dial, 2, 0);
    lv_obj_set_style_border_color(T.dial, lv_color_hex(EDGE), 0);
    lv_obj_set_style_bg_color(T.dial, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(T.dial, LV_OPA_COVER, 0);
    lv_obj_t *ring = box(T.dial, DIAL / 2 - 30, DIAL / 2 - 30, 60, 60, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x505050), 0);
    lv_obj_t *h = box(T.dial, 20, DIAL / 2, DIAL - 40, 1);
    lv_obj_set_style_bg_color(h, lv_color_hex(EDGE), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_t *v = box(T.dial, DIAL / 2, 20, 1, DIAL - 40);
    lv_obj_set_style_bg_color(v, lv_color_hex(EDGE), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    T.bubble = box(T.dial, DIAL / 2 - BUBBLE / 2, DIAL / 2 - BUBBLE / 2, BUBBLE, BUBBLE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(T.bubble, lv_color_hex(ACC.glow), 0);
    lv_obj_set_style_bg_opa(T.bubble, 220, 0);
    // Standing on an edge: a line that stays level with the world.
    T.horizon = lv_line_create(p);
    lv_obj_set_style_line_width(T.horizon, 4, 0);
    lv_obj_set_style_line_rounded(T.horizon, true, 0);
    lv_obj_set_style_line_color(T.horizon, lv_color_hex(ACC.glow), 0);
    lv_obj_add_flag(T.horizon, LV_OBJ_FLAG_HIDDEN);
    T.level_big = label(p, "--", 0, 252, 480, TEXT, 40, true, LV_TEXT_ALIGN_CENTER);
    T.level_note = label(p, "", 40, 304, 400, TEXT3, 15, false, LV_TEXT_ALIGN_CENTER);
}

void update_level()
{
    float x, y, z;
    if (!T.hooks.gravity || !T.hooks.gravity(&x, &y, &z)) {
        lv_label_set_text(T.level_big, "--");
        lv_label_set_text(T.level_note, "No accelerometer");
        return;
    }
    T.lx += 0.35f * (x - T.lx);
    T.ly += 0.35f * (y - T.ly);
    const float flat = sqrtf(T.lx * T.lx + T.ly * T.ly);  // in-plane gravity: 0 flat, 1 upright
    char big[24], note[64];
    bool level;
    if (flat < 0.7f) {  // lying down: a bubble, which floats to the high side
        lv_obj_remove_flag(T.dial, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(T.horizon, LV_OBJ_FLAG_HIDDEN);
        const float ax = asinf(std::clamp(T.lx, -1.0f, 1.0f)) * 180 / float(M_PI);
        const float ay = asinf(std::clamp(T.ly, -1.0f, 1.0f)) * 180 / float(M_PI);
        const float reach = DIAL / 2 - BUBBLE / 2 - 6;
        float bx = -ax / 20.0f * reach, by = -ay / 20.0f * reach;
        const float d = sqrtf(bx * bx + by * by);
        if (d > reach) {
            bx *= reach / d, by *= reach / d;
        }
        lv_obj_set_pos(T.bubble, int(DIAL / 2 - BUBBLE / 2 + bx), int(DIAL / 2 - BUBBLE / 2 + by));
        const float tilt = sqrtf(ax * ax + ay * ay);
        level = tilt < 1.0f;
        snprintf(big, sizeof(big), "%.1f\xC2\xB0", double(tilt));
        snprintf(note, sizeof(note), "Left-right %.1f\xC2\xB0 \xC2\xB7 front-back %.1f\xC2\xB0", double(ax), double(ay));
    } else {  // on an edge: the bottom edge's angle from level
        lv_obj_add_flag(T.dial, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(T.horizon, LV_OBJ_FLAG_HIDDEN);
        const float a = atan2f(T.lx, T.ly);  // 0 when the bottom edge is level
        const float c = cosf(a), s = sinf(a), r = 170;
        T.horizon_pts[0] = {lv_value_precise_t(240 - r * c), lv_value_precise_t(124 + r * s)};
        T.horizon_pts[1] = {lv_value_precise_t(240 + r * c), lv_value_precise_t(124 - r * s)};
        lv_line_set_points(T.horizon, T.horizon_pts, 2);
        const float deg = fabsf(a * 180 / float(M_PI));
        level = deg < 0.5f;
        snprintf(big, sizeof(big), "%.1f\xC2\xB0", double(deg));
        strlcpy(note, "The bottom edge against level", sizeof(note));
    }
    lv_label_set_text(T.level_big, big);
    lv_label_set_text(T.level_note, note);
    const uint32_t c = level ? OK : TEXT;
    lv_obj_set_style_text_color(T.level_big, lv_color_hex(c), 0);
    lv_obj_set_style_bg_color(T.bubble, lv_color_hex(level ? OK : ACC.glow), 0);
    lv_obj_set_style_line_color(T.horizon, lv_color_hex(level ? OK : ACC.glow), 0);
    if (level && !T.was_level && T.hooks.haptic) {
        T.hooks.haptic();
    }
    T.was_level = level;
}

// ---- compass ------------------------------------------------------------------------------
constexpr int ROSE = 236;
const char *const POINTS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

void load_cal()
{
    nvs_handle_t h;
    if (nvs_open("tools", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(T.mmin);
    nvs_get_blob(h, "magmin", T.mmin, &n);
    n = sizeof(T.mmax);
    nvs_get_blob(h, "magmax", T.mmax, &n);
    nvs_close(h);
}

void save_cal()
{
    nvs_handle_t h;
    if (nvs_open("tools", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "magmin", T.mmin, sizeof(T.mmin));
    nvs_set_blob(h, "magmax", T.mmax, sizeof(T.mmax));
    nvs_commit(h);
    nvs_close(h);
}

void place_letters(float heading);

void build_compass(lv_obj_t *p)
{
    T.rose = box(p, 240 - ROSE / 2, 6, ROSE, ROSE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(T.rose, 2, 0);
    lv_obj_set_style_border_color(T.rose, lv_color_hex(EDGE), 0);
    lv_obj_set_style_bg_color(T.rose, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(T.rose, LV_OPA_COVER, 0);
    for (int i = 0; i < 4; ++i) {
        T.letters[i] = label(T.rose, POINTS[i * 2], 0, 0, 40, i == 0 ? RED : TEXT2, 22, true, LV_TEXT_ALIGN_CENTER);
    }
    // The board's heading marker at the top of the dial.
    lv_obj_t *tick = box(p, 236, 0, 8, 22, 4);
    lv_obj_set_style_bg_color(tick, lv_color_hex(ACC.glow), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    T.heading = label(p, "--", 0, 252, 480, TEXT, 40, true, LV_TEXT_ALIGN_CENTER);
    T.compass_note = label(p, "", 40, 304, 400, TEXT3, 15, false, LV_TEXT_ALIGN_CENTER);
    place_letters(0);
    load_cal();
}

// The rose turns against the heading: N stays pointing north.
void place_letters(float heading)
{
    const float a = -heading * float(M_PI) / 180;
    for (int i = 0; i < 4; ++i) {
        const float t = a + i * float(M_PI) / 2, r = ROSE / 2 - 26;
        lv_obj_set_pos(T.letters[i], int(ROSE / 2 + r * sinf(t) - 20), int(ROSE / 2 - r * cosf(t) - 14));
    }
}

void update_compass()
{
    if (!T.mag_tried) {
        T.mag_tried = true;
        T.mag = bsp_magnetometer_init(BSP_MAGNETOMETER_0) == ESP_OK;
        ESP_LOGI(TAG, "magnetometer %s", T.mag ? "ready" : "unavailable");
    }
    struct bmm150_mag_data m = {};
    if (!T.mag || bsp_magnetometer_read(BSP_MAGNETOMETER_0, &m) != ESP_OK) {
        lv_label_set_text(T.heading, "--");
        lv_label_set_text(T.compass_note, "No magnetometer");
        return;
    }
    // Hard-iron offset from the range seen while the board is turned around.
    float v[3] = {float(m.x), float(m.y), float(m.z)};
    bool grew = false;
    for (int i = 0; i < 3; ++i) {
        if (v[i] < T.mmin[i]) {
            T.mmin[i] = v[i], grew = true;
        }
        if (v[i] > T.mmax[i]) {
            T.mmax[i] = v[i], grew = true;
        }
    }
    static int since_save = 0;
    if (grew && ++since_save > 40) {
        since_save = 0, save_cal();
    }
    const float span = std::min(T.mmax[0] - T.mmin[0], T.mmax[1] - T.mmin[1]);
    float x = v[0] - (T.mmin[0] + T.mmax[0]) / 2, y = v[1] - (T.mmin[1] + T.mmax[1]) / 2, z = v[2] - (T.mmin[2] + T.mmax[2]) / 2;
    if (T.hooks.to_ui) {
        T.hooks.to_ui(&x, &y, &z);
    }
    // Flat: the field's horizontal part points north; the heading is the
    // angle from north to the top of the screen, clockwise.
    float h = -atan2f(x, -y) * 180 / float(M_PI);
    h = fmodf(h + 360.0f, 360.0f);
    float d = h - T.head;
    if (d > 180) {
        d -= 360;
    }
    if (d < -180) {
        d += 360;
    }
    T.head = fmodf(T.head + 0.3f * d + 360.0f, 360.0f);
    char text[16];
    snprintf(text, sizeof(text), "%d\xC2\xB0 %s", int(lroundf(T.head)) % 360, POINTS[int(lroundf(T.head / 45)) % 8]);
    lv_label_set_text(T.heading, text);
    lv_label_set_text(T.compass_note, span < 60 ? "Turn the board around slowly, flat, to calibrate."
                      : "Hold it flat. Keep it away from magnets and metal.");
    place_letters(T.head);
}

// ---- USB webcam ---------------------------------------------------------------------------
void update_webcam()
{
    char text[96] = "Not available";
    const bool on = T.hooks.webcam_status && T.hooks.webcam_status(text, sizeof(text));
    lv_obj_set_style_bg_color(T.cam_btn, lv_color_hex(on ? ACC.glow : FIELD), 0);
    lv_obj_set_style_line_color(lv_obj_get_child(T.cam_btn, 0), lv_color_hex(on ? 0x2A0A04 : TEXT), 0);
    lv_label_set_text(T.cam_label, on ? "On" : "Off");
    lv_label_set_text(T.cam_note, text);
}

void webcam_cb(lv_event_t *)
{
    if (!T.hooks.webcam || !T.hooks.webcam_status) {
        return;
    }
    char text[96];
    T.hooks.webcam(!T.hooks.webcam_status(text, sizeof(text)));
    update_webcam();
}

void build_webcam(lv_obj_t *p)
{
    T.cam_btn = round_button(p, 165, 8, 150, FIELD);
    icon(T.cam_btn, "camera", 40, 40, 70, TEXT, 1.7f);
    lv_obj_add_event_cb(T.cam_btn, webcam_cb, LV_EVENT_CLICKED, nullptr);
    // The torch, beside it: light for the camera while it streams (the same as the Torch tab's).
    T.cam_torch = round_button(p, 336, 48, 72, FIELD);
    icon(T.cam_torch, "torch", 18, 18, 36, TEXT, 1.7f);
    lv_obj_add_event_cb(T.cam_torch, torch_cb, LV_EVENT_CLICKED, nullptr);
    T.cam_label = label(p, "Off", 0, 168, 480, TEXT, 26, true, LV_TEXT_ALIGN_CENTER);
    T.cam_note = label(p, "", 30, 206, 420, TEXT2, 15, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *n = label(p, "A USB camera for the computer, 1280 \xC3\x97 720 or 640 \xC3\x97 480, upright whichever way "
                        "the board is held; the small button lights the flash. While it is on, the USB port is only a "
                        "camera: no installs, logs or Wi-Fi tools.",
                        50, 240, 380, TEXT3, 14, false, LV_TEXT_ALIGN_CENTER);
    wrap_label(n);
    update_webcam();
}

// ---- calculator ---------------------------------------------------------------------------
double entry_value()
{
    return strtod(T.entry.c_str(), nullptr);
}

std::string show(double v)
{
    if (!isfinite(v)) {
        return "Error";
    }
    char text[32];
    if (fabs(v) >= 1e12 || (fabs(v) < 1e-6 && v != 0)) {
        snprintf(text, sizeof(text), "%.6g", v);
    } else {
        snprintf(text, sizeof(text), "%.10f", v);
        char *dot = strchr(text, '.');
        if (dot) {
            char *end = text + strlen(text) - 1;
            while (end > dot && *end == '0') {
                *end-- = 0;
            }
            if (end == dot) {
                *end = 0;
            }
        }
    }
    return strcmp(text, "-0") ? text : "0";
}

void calc_show()
{
    lv_label_set_text(T.display, T.entry.c_str());
    const int px = T.entry.size() > 12 ? 30 : T.entry.size() > 9 ? 38 : 48;
    lv_obj_set_style_text_font(T.display, font(px, true), 0);
    char e[48] = "";
    if (T.op) {
        snprintf(e, sizeof(e), "%s %c", show(T.acc).c_str(), T.op);
    }
    lv_label_set_text(T.expr, e);
}

double apply(double a, char op, double b)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b == 0 ? NAN : a / b;
    default: return b;
    }
}

void key(const char *k)
{
    const char c = k[0];
    if (isdigit(uint8_t(c)) || c == '.') {
        if (T.fresh) {
            T.entry = c == '.' ? "0" : "", T.fresh = false;
        }
        if (c == '.' && T.entry.find('.') != std::string::npos) {
            return;
        }
        if (T.entry == "0" && c != '.') {
            T.entry.clear();
        }
        if (T.entry.size() < 15) {
            T.entry += c;
        }
    } else if (!strcmp(k, "C")) {
        T.entry = "0", T.acc = 0, T.op = 0, T.fresh = true;
    } else if (!strcmp(k, "\xC2\xB1")) {
        T.entry = T.entry[0] == '-' ? T.entry.substr(1) : (T.entry == "0" ? T.entry : "-" + T.entry);
    } else if (!strcmp(k, "%")) {
        T.entry = show(entry_value() / 100), T.fresh = true;
    } else if (!strcmp(k, LV_SYMBOL_BACKSPACE)) {  // backspace
        if (!T.fresh && T.entry.size() > 1) {
            T.entry.pop_back();
        } else {
            T.entry = "0", T.fresh = true;
        }
    } else if (!strcmp(k, "=")) {
        if (T.op) {
            T.entry = show(apply(T.acc, T.op, entry_value()));
        }
        T.op = 0, T.fresh = true;
    } else {  // an operator
        const char op = !strcmp(k, "\xC3\x97") ? '*' : !strcmp(k, "\xC3\xB7") ? '/' : !strcmp(k, "\xE2\x88\x92") ? '-' : '+';
        if (T.op && !T.fresh) {
            T.acc = apply(T.acc, T.op, entry_value());
        } else if (!T.op) {
            T.acc = entry_value();
        }
        T.op = op, T.fresh = true;
        T.entry = show(T.acc);
    }
    calc_show();
}

void key_cb(lv_event_t *e)
{
    if (T.hooks.haptic) {
        T.hooks.haptic();
    }
    key(static_cast<const char *>(lv_event_get_user_data(e)));
}

void build_calc(lv_obj_t *p)
{
    T.expr = label(p, "", 50, 0, 380, TEXT3, 17, true, LV_TEXT_ALIGN_RIGHT);
    T.display = label(p, "0", 50, 20, 380, TEXT, 48, true, LV_TEXT_ALIGN_RIGHT);
    static const char *const KEYS[5][4] = {{"C", "\xC2\xB1", "%", "\xC3\xB7"},
        {"7", "8", "9", "\xC3\x97"},
        {"4", "5", "6", "\xE2\x88\x92"},
        {"1", "2", "3", "+"},
        {"0", ".", LV_SYMBOL_BACKSPACE, "="}
    };
    constexpr int KW = 88, KH = 48, GX = 8, GY = 7, X0 = 240 - (4 * KW + 3 * GX) / 2, Y0 = 82;
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 4; ++c) {
            const char *k = KEYS[r][c];
            const bool op = c == 3, top = r == 0 && c < 3;
            lv_obj_t *b = box(p, X0 + c * (KW + GX), Y0 + r * (KH + GY), KW, KH, KH / 2);
            lv_obj_set_style_bg_color(b, lv_color_hex(op ? ACC.lo : top ? 0x505055 : 0x2A2A2D), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            press_fx(b);
            label(b, k, 0, 9, KW, op ? 0xFFFFFF : TEXT, 24, true, LV_TEXT_ALIGN_CENTER);
            lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, const_cast<char *>(k));
        }
    calc_show();
}

// ---- screen -------------------------------------------------------------------------------
void show_tool(int t)
{
    T.tool = std::clamp(t, 0, TOOLS - 1);
    for (int i = 0; i < TOOLS; ++i) {
        if (i == T.tool) {
            lv_obj_remove_flag(T.pane[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(T.pane[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (T.tabs && capsule_selected(T.tabs) != T.tool) {
        capsule_select(T.tabs, T.tool, true);
    }
    interact::set_visible(T.active && T.tool == BOARD);
    nvs_handle_t h;
    if (nvs_open("tools", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "tool", uint8_t(T.tool));
        nvs_commit(h);
        nvs_close(h);
    }
}

void tab_cb(lv_event_t *e)
{
    show_tool(capsule_selected(lv_event_get_current_target_obj(e)));
}

void tick(lv_timer_t *)
{
    if (!T.active) {
        return;
    }
    if (T.tool == LEVEL) {
        update_level();
    } else if (T.tool == COMPASS) {
        update_compass();
    } else if (T.tool == BOARD) {
        interact::update();
    } else if (T.tool == WEBCAM && ++T.cam_tick % 8 == 0) {
        update_webcam();
    }
}

void build(lv_obj_t *s)
{
    T.screen = s;
    lv_obj_t *back = box(s, 56, 14, 44, 44, 22);
    lv_obj_set_style_bg_color(back, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    icon(back, "back", 10, 10, 24, TEXT, 2.0f);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    press_fx(back);
    lv_obj_add_event_cb(back, [](lv_event_t *) {
        if (T.hooks.exit) {
            T.hooks.exit();
        }
    }, LV_EVENT_CLICKED, nullptr);
    label(s, "Tools", 112, 20, 200, TEXT, 26, true);
    T.tabs = capsule(s, 30, 66, 420, 44, NAMES, TOOLS, T.tool, ACC, 14);
    lv_obj_add_event_cb(T.tabs, tab_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    for (int i = 0; i < TOOLS; ++i) {
        T.pane[i] = box(s, 0, 124, PANEL, 356);
    }
    build_torch(T.pane[TORCH]);
    build_level(T.pane[LEVEL]);
    build_compass(T.pane[COMPASS]);
    build_calc(T.pane[CALC]);
    interact::build(T.pane[BOARD], ACC);
    build_webcam(T.pane[WEBCAM]);
    show_tool(T.tool);
    T.timer = lv_timer_create(tick, 60, nullptr);
    T.built = true;
}
}  // namespace

extern "C" void tools_app_init(const tools_hooks_t *hooks)
{
    if (hooks) {
        T.hooks = *hooks;
    }
    nvs_handle_t h;
    if (nvs_open("tools", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "tool", &v) == ESP_OK && v < TOOLS) {
            T.tool = v;
        }
        nvs_close(h);
    }
    interact::start();  // a saved light mode runs without the app open
}

extern "C" void tools_app_open(lv_obj_t *screen)
{
    if (!T.built) {
        build(screen);
    }
    tools_app_set_active(true);
}

extern "C" void tools_app_set_active(bool active)
{
    T.active = active && T.built;
    interact::set_visible(T.active && T.tool == BOARD);
    if (!T.active && T.built) {
        if (T.screen_light) {
            light_off_cb(nullptr);
        }
        if (T.torch_on && T.hooks.torch) {
            T.hooks.torch(false), T.torch_on = false, torch_look();
        }
    }
}

extern "C" void tools_app_select(int tool)
{
    if (T.built) {
        show_tool(tool);
    } else {
        T.tool = std::clamp(tool, 0, TOOLS - 1);
    }
}
