// SPDX-License-Identifier: Apache-2.0
// The Interaction app: the Interaction sub-board, in either slot: two buttons
// (touch pads, or keys to ground), PIR motion, a light sensor, six RGB LEDs and
// an infrared LED (here a remote for the LG TV, or any NEC code for Swifty).
// Like the stock Mosaico-Claw app, plus light modes of its own: buttons and
// motion shown in colour, a rainbow, a night light that comes on with motion
// in the dark, and a fixed colour.
// A worker task owns the board (opening it blocks while the slot is probed, and
// the driver's calls are not for the LVGL task): it looks for the board while
// the pane shows, a light mode is on, or Swifty asks, and keeps it until it is
// pulled out. The pane and Swifty read its snapshot and post requests.
#include "interact.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mosaico_module_interact.h"
#include "mosaico_module_mgr.h"
#include "nvs.h"
#include "tools_app.h"

namespace interact {
namespace {
using namespace mui;
const char *TAG = "interact";
constexpr int LEDS = MOSAICO_INTERACT_LED_COUNT;
enum Mode { OFF, REACT, RAINBOW, NIGHT, COLOUR, MODES };
const char *const MODE_NAMES[MODES] = {"Off", "React", "Rainbow", "Night", "Colour"};
const char *const MODE_KEYS[MODES] = {"off", "react", "rainbow", "night", "colour"};
constexpr uint32_t PALETTE[] = {0xFF8A40, 0xFF3020, 0x30FF50, 0x3070FF, 0xA050FF, 0xFFFFFF, 0x30E0FF, 0xFF50A0};
constexpr int64_t NIGHT_US = 30 * 1000000LL;  // the night light stays on this long after motion
constexpr int NIGHT_DARK = 15;                 // light level (0-100, relative) below which it is night

struct Snapshot {
    bool present = false, touch = false, left = false, right = false, motion = false;
    int slot = -1, light = -1, light_raw = 0;
    int64_t motion_at = 0;  // the last motion, esp_timer microseconds (0: none yet)
    uint32_t leds[LEDS] = {};
};

std::mutex lock;
Snapshot snap;
std::atomic<bool> visible{false};
std::atomic<bool> gaming{false};  // a game is up: poll fast, its flashes on the lights
std::atomic<uint32_t> flash_colour{0};
std::atomic<int64_t> flash_at{0};
constexpr int64_t FLASH_US = 450000;
std::atomic<int64_t> wanted_until{0};  // Swifty asked: look for the board until then
std::atomic<int> mode{OFF};
std::atomic<uint32_t> colour{PALETTE[0]};
std::atomic<uint32_t> ir_request{0};  // 0x10000 | address << 8 | command
std::atomic<uint32_t> ir_asked{0}, ir_answered{0};
std::atomic<int> ir_result{0};  // the last send: 1 sent, -1 failed
TaskHandle_t task = nullptr;

Snapshot read_snapshot()
{
    std::lock_guard<std::mutex> g(lock);
    return snap;
}

bool searching()
{
    return visible || gaming || mode != OFF || esp_timer_get_time() < wanted_until;
}

uint32_t hue_rgb(float h)  // h in degrees, full saturation and value
{
    h = fmodf(h, 360.0f) / 60.0f;
    const int i = int(h);
    const float f = h - i;
    const uint8_t q = uint8_t(255 * (1 - f)), t = uint8_t(255 * f);
    switch (i) {
    case 0: return 0xFF0000 | t << 8;
    case 1: return q << 16 | 0xFF00;
    case 2: return 0xFF00 | t;
    case 3: return q << 8 | 0xFF;
    case 4: return t << 16 | 0xFF;
    default: return 0xFF0000 | q;
    }
}

uint32_t scale(uint32_t c, float k)
{
    k = std::clamp(k, 0.0f, 1.0f);
    return uint32_t(((c >> 16) & 0xFF) * k) << 16 | uint32_t(((c >> 8) & 0xFF) * k) << 8 | uint32_t((c & 0xFF) * k);
}

void save_mode()
{
    nvs_handle_t h;
    if (nvs_open("tools", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "ledmode", uint8_t(mode.load()));
    nvs_set_u32(h, "ledrgb", colour.load());
    nvs_commit(h);
    nvs_close(h);
}

void worker(void *)
{
    mosaico_interact_handle_t board = nullptr;
    mosaico_interact_info_t info = {};
    int64_t next_try = 0, night_until = 0;
    uint32_t shown[LEDS] = {};
    bool shown_valid = false;
    float hue = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (!board) {
            if (!searching() || now < next_try) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            // Asked for (the pane, Swifty): wait for discovery; else a short look now and then.
            const bool asked = visible || now < wanted_until;
            mosaico_interact_config_t cfg = MOSAICO_INTERACT_DEFAULT_CONFIG();
            cfg.discovery_timeout_ms = asked ? 1500 : 300;
            cfg.led_brightness = 96;
            const esp_err_t err = mosaico_interact_open(&cfg, &board);
            if (err == ESP_OK && mosaico_interact_get_info(board, &info) != ESP_OK) {
                mosaico_interact_close(board);
                board = nullptr;
            } else if (err != ESP_OK) {
                if (board) {
                    mosaico_interact_close(board);    // a half-open board
                }
                board = nullptr;
                if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_NOT_FOUND) {
                    ESP_LOGW(TAG, "open: %s", esp_err_to_name(err));
                }
            }
            if (!board) {
                next_try = esp_timer_get_time() + (asked ? 1000000 : 5000000);
                continue;
            }
            ESP_LOGI(TAG, "Interaction board in the %s slot, %s buttons", mosaico_module_mgr_slot_to_name(info.slot),
                     info.button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH ? "touch" : "push");
            {
                std::lock_guard<std::mutex> g(lock);
                snap = Snapshot();
                snap.present = true;
                snap.slot = int(info.slot);
                snap.touch = info.button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
            }
            shown_valid = false;
        }

        // Pulled out: let it go (and look again while wanted).
        mosaico_module_mgr_info_t m = {};
        if (mosaico_module_mgr_get_info(info.slot, &m) == ESP_OK && m.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
            mosaico_interact_close(board);
            board = nullptr;
            {
                std::lock_guard<std::mutex> g(lock);
                snap = Snapshot();
            }
            ESP_LOGI(TAG, "Interaction board removed");
            next_try = 0;
            continue;
        }

        if (const uint32_t req = ir_request.exchange(0)) {
            const esp_err_t err = mosaico_interact_ir_send_nec(board, uint8_t(req >> 8), uint8_t(req));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "IR 0x%02X/0x%02X: %s", unsigned((req >> 8) & 0xFF), unsigned(req & 0xFF), esp_err_to_name(err));
            }
            ir_result = err == ESP_OK ? 1 : -1;
            ir_answered = ir_asked.load();
        }

        mosaico_interact_inputs_t in = {};
        const bool read = mosaico_interact_read_inputs(board, &in) == ESP_OK;
        if (read) {
            std::lock_guard<std::mutex> g(lock);
            snap.left = in.left_pressed;
            snap.right = in.right_pressed;
            snap.motion = in.motion_detected;
            if (in.motion_detected) {
                snap.motion_at = now;
            }
            snap.light = in.light_level;
            snap.light_raw = in.light_raw;
        }

        uint32_t frame[LEDS] = {};
        switch (mode.load()) {
        case REACT: {  // as Espressif's demo: left red, motion green, right blue
            const uint32_t c = (in.left_pressed ? 0xFF0000 : 0) | (in.motion_detected ? 0x00FF00 : 0) | (in.right_pressed ? 0x0000FF : 0);
            std::fill(frame, frame + LEDS, c);
            break;
        }
        case RAINBOW:
            hue = fmodf(hue + 9.0f, 360.0f);
            for (int i = 0; i < LEDS; ++i) {
                frame[i] = hue_rgb(hue + i * (360.0f / LEDS));
            }
            break;
        case NIGHT:
            if (read && in.motion_detected && in.light_level < NIGHT_DARK) {
                night_until = now + NIGHT_US;
            }
            if (now < night_until) {
                const float left = float(night_until - now) / 3e6f;  // fades over its last three seconds
                std::fill(frame, frame + LEDS, scale(0xFFA040, std::min(1.0f, left)));
            }
            break;
        case COLOUR: std::fill(frame, frame + LEDS, colour.load()); break;
        default: break;
        }
        if (gaming && now - flash_at.load() < FLASH_US) {  // a game's flash, fading
            const float left = 1 - float(now - flash_at.load()) / float(FLASH_US);
            std::fill(frame, frame + LEDS, scale(flash_colour.load(), left * left));
        }
        if (!shown_valid || memcmp(frame, shown, sizeof(frame))) {
            mosaico_interact_rgb_t rgb[LEDS];
            for (int i = 0; i < LEDS; ++i) rgb[i] = {uint8_t(frame[i] >> 16), uint8_t(frame[i] >> 8), uint8_t(frame[i])};
            if (mosaico_interact_led_write(board, rgb, LEDS) == ESP_OK) {
                memcpy(shown, frame, sizeof(frame));
                shown_valid = true;
                std::lock_guard<std::mutex> g(lock);
                memcpy(snap.leds, frame, sizeof(frame));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(gaming ? 30 : 100));
    }
}

// ---- requests (any task) -------------------------------------------------------------------
void set_mode(int m, uint32_t c)
{
    mode = std::clamp(m, 0, MODES - 1);
    if (c) {
        colour = c;
    }
    save_mode();
}

// Sends an NEC code and waits for the worker (false: no board, or it failed).
bool send_ir(uint8_t address, uint8_t command, int wait_ms)
{
    wanted_until = esp_timer_get_time() + 10 * 1000000LL;
    const uint32_t n = ++ir_asked;
    ir_request = 0x10000u | uint32_t(address) << 8 | command;
    for (int waited = 0; waited < wait_ms; waited += 50) {
        if (ir_answered.load() == n) {
            return ir_result == 1;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

// Swifty: wait a little for the board when it was not open yet.
Snapshot settle(int wait_ms)
{
    wanted_until = esp_timer_get_time() + 10 * 1000000LL;
    Snapshot s = read_snapshot();
    for (int waited = 0; !s.present && waited < wait_ms; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s = read_snapshot();
    }
    return s;
}

// LG TV keys (NEC, address 0x04).
struct Key {
    const char *name;
    uint8_t command;
};
constexpr Key LG_KEYS[] = {{"power", 0x08},   {"volume_up", 0x02}, {"volume_down", 0x03}, {"mute", 0x09},
    {"input", 0x0B},   {"ok", 0x44},        {"up", 0x40},          {"down", 0x41},
    {"left", 0x07},    {"right", 0x06},     {"back", 0x28},        {"home", 0x7C},
    {"channel_up", 0x00}, {"channel_down", 0x01}
};
constexpr uint8_t LG_ADDRESS = 0x04;

// ---- the pane (LVGL task) ------------------------------------------------------------------
struct {
    const Accent *acc = nullptr;
    lv_obj_t *status = nullptr, *card[3] = {}, *lamp[3] = {}, *note[3] = {}, *light = nullptr, *light_bar = nullptr,
                                                                         *modes = nullptr, *leds[LEDS] = {}, *remote = nullptr;
    int64_t remote_note_until = 0;
    uint32_t waiting_for = 0;  // an IR send from the pane whose answer is still to show
} U;

void mode_cb(lv_event_t *e)
{
    set_mode(capsule_selected(lv_event_get_current_target_obj(e)), 0);
}

// Tapping the lights: the next colour (and the colour mode).
void leds_cb(lv_event_t *)
{
    const uint32_t c = colour.load();
    size_t i = 0;
    while (i < sizeof(PALETTE) / sizeof(*PALETTE) && PALETTE[i] != c) {
        ++i;
    }
    set_mode(COLOUR, PALETTE[(i + 1) % (sizeof(PALETTE) / sizeof(*PALETTE))]);
}

void remote_cb(lv_event_t *e)
{
    const uint8_t command = uint8_t(uintptr_t(lv_event_get_user_data(e)));
    wanted_until = esp_timer_get_time() + 10 * 1000000LL;
    U.waiting_for = ++ir_asked;
    ir_request = 0x10000u | uint32_t(LG_ADDRESS) << 8 | command;
    set_text(U.remote, "TV remote (LG) \xC2\xB7 sending\xE2\x80\xA6");
    U.remote_note_until = esp_timer_get_time() + 3000000;
}

lv_obj_t *card(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *c = box(p, x, y, w, h, 22);
    set_bg_color(c, lv_color_hex(CARD));
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(EDGE), 0);
    return c;
}

void set_lamp(lv_obj_t *lamp, bool on, uint32_t colour_on)
{
    set_bg_color(lamp, lv_color_hex(on ? colour_on : OFF));
    set_shadow_color(lamp, lv_color_hex(colour_on));
    set_shadow_opa(lamp, on ? 160 : 0);
}
}  // namespace

void start()
{
    if (task) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("tools", NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        uint32_t c = 0;
        if (nvs_get_u8(h, "ledmode", &m) == ESP_OK && m < MODES) {
            mode = m;
        }
        if (nvs_get_u32(h, "ledrgb", &c) == ESP_OK && c) {
            colour = c;
        }
        nvs_close(h);
    }
    // Only the driver's calls: the stack can live in PSRAM. Below LVGL's draw tasks.
    xTaskCreatePinnedToCoreWithCaps(worker, "interact", 6144, nullptr, 2, &task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void set_visible(bool v)
{
    visible = v;
}

void build(lv_obj_t *p, const Accent &a)
{
    U.acc = &a;
    U.status = label(p, "", 20, 0, 440, TEXT2, 15, true, LV_TEXT_ALIGN_CENTER);
    static const char *const TITLES[3] = {"Left", "Motion", "Right"};
    for (int i = 0; i < 3; ++i) {
        U.card[i] = card(p, 30 + i * 144, 26, 132, 92);
        label(U.card[i], TITLES[i], 0, 10, 132, TEXT, 16, true, LV_TEXT_ALIGN_CENTER);
        U.lamp[i] = box(U.card[i], 66 - 13, 36, 26, 26, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(U.lamp[i], LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(U.lamp[i], 18, 0);
        lv_obj_set_style_border_width(U.lamp[i], 2, 0);  // visible when off too
        lv_obj_set_style_border_color(U.lamp[i], lv_color_hex(0x4A4A4E), 0);
        set_lamp(U.lamp[i], false, a.glow);
        U.note[i] = label(U.card[i], "", 4, 68, 124, TEXT3, 13, false, LV_TEXT_ALIGN_CENTER);
    }
    lv_obj_t *lc = card(p, 30, 126, 420, 56);
    label(lc, "Light", 18, 8, 120, TEXT, 16, true);
    U.light = label(lc, "--", 240, 9, 162, TEXT2, 15, true, LV_TEXT_ALIGN_RIGHT);
    U.light_bar = bar(lc, 18, 36, 384, 8, 0, a);
    U.modes = capsule(p, 30, 192, 420, 38, MODE_NAMES, MODES, mode, a, 14);
    lv_obj_add_event_cb(U.modes, mode_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    // The six lights as the board shows them; tap for the next colour.
    lv_obj_t *strip = box(p, 130, 236, 220, 30, 15);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(strip, leds_cb, LV_EVENT_CLICKED, nullptr);
    for (int i = 0; i < LEDS; ++i) {
        U.leds[i] = box(strip, 10 + i * 36, 5, 20, 20, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(U.leds[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(U.leds[i], 1, 0);
        lv_obj_set_style_border_color(U.leds[i], lv_color_hex(EDGE), 0);
        lv_obj_set_style_shadow_width(U.leds[i], 12, 0);
        lv_obj_remove_flag(U.leds[i], LV_OBJ_FLAG_CLICKABLE);
    }
    U.remote = label(p, "TV remote (LG)", 20, 272, 440, TEXT3, 14, true, LV_TEXT_ALIGN_CENTER);
    struct Button {
        const char *icon;
        uint8_t command;
    };
    static const Button BUTTONS[4] = {{"power", 0x08}, {"voldown", 0x03}, {"volup", 0x02}, {"speaker", 0x09}};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *b = box(p, 123 + i * 62, 294, 48, 48, LV_RADIUS_CIRCLE);
        set_bg_color(b, lv_color_hex(i == 0 ? a.lo : FIELD));
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        press_fx(b);
        icon(b, BUTTONS[i].icon, 12, 12, 24, i == 0 ? 0xFFFFFF : TEXT, 1.9f);
        lv_obj_add_event_cb(b, remote_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(uintptr_t(BUTTONS[i].command)));
    }
    update();
}

void update()
{
    if (!U.status) {
        return;
    }
    const Snapshot s = read_snapshot();
    const int64_t now = esp_timer_get_time();
    char text[96];
    if (s.present) {
        snprintf(text, sizeof(text), "Interaction board \xC2\xB7 %s slot \xC2\xB7 %s buttons",
                 s.slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "left" : "right", s.touch ? "touch" : "push");
    } else {
        strlcpy(text, "Plug the Interaction board into either side", sizeof(text));
    }
    set_text(U.status, text);
    const bool on[3] = {s.present && s.left, s.present && s.motion, s.present && s.right};
    const uint32_t colours[3] = {0xFF5050, 0x60E080, 0x5090FF};
    for (int i = 0; i < 3; ++i) {
        set_lamp(U.lamp[i], on[i], colours[i]);
    }
    set_text(U.note[0], !s.present ? "" : s.left ? "Pressed" : "Touch or press");
    set_text(U.note[2], !s.present ? "" : s.right ? "Pressed" : "Touch or press");
    if (!s.present) {
        set_text(U.note[1], "");
    } else if (s.motion) {
        set_text(U.note[1], "Now");
    } else if (!s.motion_at) {
        set_text(U.note[1], "None yet");
    } else {
        const int ago = int((now - s.motion_at) / 1000000);
        if (ago < 60) {
            snprintf(text, sizeof(text), "%d s ago", ago);
        } else if (ago < 3600) {
            snprintf(text, sizeof(text), "%d min ago", ago / 60);
        } else {
            snprintf(text, sizeof(text), "%d h ago", ago / 3600);
        }
        set_text(U.note[1], text);
    }
    if (s.present && s.light >= 0) {
        snprintf(text, sizeof(text), "%d%%%s", s.light, s.light < NIGHT_DARK ? " \xC2\xB7 dark" : "");
        set_text(U.light, text);
        bar_set(U.light_bar, s.light);
    } else {
        set_text(U.light, "--");
        bar_set(U.light_bar, 0);
    }
    if (capsule_selected(U.modes) != mode.load()) {
        capsule_select(U.modes, mode.load(), true);
    }
    for (int i = 0; i < LEDS; ++i) {
        const uint32_t c = s.present ? s.leds[i] : 0;
        set_bg_color(U.leds[i], lv_color_hex(c ? c : 0x202020));
        set_shadow_color(U.leds[i], lv_color_hex(c));
        set_shadow_opa(U.leds[i], c ? 140 : 0);
    }
    if (U.waiting_for && ir_answered.load() == U.waiting_for) {
        U.waiting_for = 0;
        set_text(U.remote, ir_result == 1 ? "TV remote (LG) \xC2\xB7 sent" : "TV remote (LG) \xC2\xB7 not sent");
        U.remote_note_until = now + 1500000;
    } else if (U.waiting_for && now > U.remote_note_until) {
        U.waiting_for = 0;
        set_text(U.remote, "TV remote (LG) \xC2\xB7 no board");
        U.remote_note_until = now + 1500000;
    } else if (!U.waiting_for && U.remote_note_until && now > U.remote_note_until) {
        U.remote_note_until = 0;
        set_text(U.remote, "TV remote (LG)");
    }
}
}  // namespace interact

// ---- Games (any task) ------------------------------------------------------------------------
extern "C" void tools_board_game(bool on)
{
    interact::gaming = on;
}

extern "C" bool tools_board_buttons(bool *left, bool *right)
{
    const interact::Snapshot s = interact::read_snapshot();
    *left = s.left, *right = s.right;
    return s.present;
}

extern "C" void tools_board_flash(uint32_t rgb)
{
    interact::flash_colour = rgb;
    interact::flash_at = esp_timer_get_time();
}

// ---- Swifty (any task) -----------------------------------------------------------------------
extern "C" void tools_board_report(char *out, size_t cap)
{
    using namespace interact;
    const Snapshot s = settle(6000);
    if (!s.present) {
        snprintf(out, cap, "No Interaction board found: it plugs into either side of the board. Lights: %s.",
                 MODE_KEYS[mode.load()]);
        return;
    }
    const int64_t now = esp_timer_get_time();
    char motion[48];
    if (s.motion) {
        strlcpy(motion, "motion now", sizeof(motion));
    } else if (!s.motion_at) {
        strlcpy(motion, "no motion seen since it was found", sizeof(motion));
    } else {
        snprintf(motion, sizeof(motion), "last motion %lld s ago", (long long)((now - s.motion_at) / 1000000));
    }
    snprintf(out, cap,
             "Interaction board in the %s slot (%s buttons): left %s, right %s; %s; light %d%% (relative, %s); "
             "lights mode %s, colour #%06lX. Its infrared LED works as an LG TV remote.",
             s.slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "left" : "right", s.touch ? "touch" : "push",
             s.left ? "pressed" : "not pressed", s.right ? "pressed" : "not pressed", motion, s.light,
             s.light < NIGHT_DARK ? "dark" : "lit", MODE_KEYS[mode.load()], (unsigned long)colour.load());
}

extern "C" bool tools_board_lights(const char *mode_name, const char *colour_name, char *out, size_t cap)
{
    using namespace interact;
    static const struct {
        const char *name;
        uint32_t rgb;
    } COLOURS[] = {{"orange", 0xFF8A40}, {"red", 0xFF3020},   {"green", 0x30FF50},  {"blue", 0x3070FF},
        {"purple", 0xA050FF}, {"white", 0xFFFFFF}, {"cyan", 0x30E0FF},   {"pink", 0xFF50A0},
        {"yellow", 0xFFD020}, {"warm", 0xFFA040},  {"warm white", 0xFFA040}
    };
    uint32_t rgb = 0;
    if (colour_name && *colour_name) {
        if (colour_name[0] == '#' && strlen(colour_name) == 7) {
            rgb = uint32_t(strtoul(colour_name + 1, nullptr, 16));
        }
        for (const auto &c : COLOURS)
            if (!strcasecmp(colour_name, c.name)) {
                rgb = c.rgb;
            }
        if (!rgb) {
            snprintf(out, cap, "Unknown colour \"%s\": use a name (red, green, blue, ...) or #RRGGBB.", colour_name);
            return false;
        }
    }
    int m = rgb && (!mode_name || !*mode_name) ? COLOUR : -1;
    for (int i = 0; mode_name && i < MODES; ++i)
        if (!strcasecmp(mode_name, MODE_KEYS[i]) || !strcasecmp(mode_name, MODE_NAMES[i])) {
            m = i;
        }
    if (mode_name && !strcasecmp(mode_name, "color")) {
        m = COLOUR;
    }
    if (m < 0) {
        snprintf(out, cap, "Unknown mode: use off, react, rainbow, night or colour.");
        return false;
    }
    set_mode(m, rgb);
    const Snapshot s = settle(4000);
    snprintf(out, cap, "Lights set to %s%s%s.%s", MODE_KEYS[m], m == COLOUR ? " " : "",
             m == COLOUR ? (colour_name && *colour_name ? colour_name : "") : "",
             s.present ? "" : " No Interaction board is plugged in yet: it will apply when one is.");
    return true;
}

extern "C" bool tools_board_ir(const char *key, int address, int command, char *out, size_t cap)
{
    using namespace interact;
    uint8_t a = LG_ADDRESS, c = 0;
    bool found = false;
    if (key && *key) {
        for (const auto &k : LG_KEYS)
            if (!strcasecmp(key, k.name)) {
                c = k.command, found = true;
            }
        if (!found) {
            std::string names;
            for (const auto &k : LG_KEYS) {
                names += names.empty() ? k.name : std::string(", ") + k.name;
            }
            snprintf(out, cap, "Unknown key \"%s\". LG TV keys: %s.", key, names.c_str());
            return false;
        }
    } else if (address >= 0 && address <= 255 && command >= 0 && command <= 255) {
        a = uint8_t(address), c = uint8_t(command), found = true;
    }
    if (!found) {
        snprintf(out, cap, "Give a key (power, volume_up, ...) or an NEC address and command (0-255).");
        return false;
    }
    const bool ok = send_ir(a, c, 4000);
    if (ok) {
        snprintf(out, cap, "Sent %s (NEC 0x%02X/0x%02X) from the Interaction board's infrared LED.", key && *key ? key : "the code", a, c);
    } else {
        snprintf(out, cap, "Could not send: %s", read_snapshot().present ? "the infrared LED failed." : "no Interaction board is plugged in.");
    }
    return ok;
}
