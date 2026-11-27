// SPDX-License-Identifier: Apache-2.0
// ESP-Mosaico shell. Owns the display and input, starts Wi-Fi from Vibe Mode
// shared information, shows the launcher and hosts Glimdock and Home, each on
// its own LVGL screen inside the round-cornered panel's visible area.
#include "mosaico_shell.h"

#include <math.h>
#include <atomic>
#include <string.h>
#include <time.h>

#include "ArduinoJson.h"
#include "Preferences.h"
#include "WiFi.h"
#include "app_config.h"
#include "boot_stage.h"
#include "camera_app.h"
#include "files_app.h"
#include "tools_app.h"
#include "usb_webcam.h"
#include "swifty_app.h"
#include "bsp/esp_mosaico.h"
#include "display_clock.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "mbedtls/ssl.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "glimdock_app.h"
#include "haptics.h"
#include "home_app.h"
#include "iot_button.h"
#include "launcher_ui.h"
#include "mbedtls/platform_util.h"
#include "mui.h"
#include "src/misc/lv_event_private.h"  // an object's event filters: is it a button?
#include "orientation.h"
#include "frame_sync.h"
#include "perf.h"
#include "sdkconfig.h"
#include "shell_internal.h"
#include "time_zone.h"
#include "web_settings.h"
#include "tusb.h"

namespace {
const char *TAG = "shell";
constexpr int PANEL = 480;
constexpr int DEFAULT_RADIUS = 80, MAX_RADIUS = 160;
constexpr const char *PREFS = "mshell";

struct {
    int radius = DEFAULT_RADIUS, brightness = 80;
    ShellApp current = ShellApp::Launcher;
    lv_obj_t *launcher = nullptr, *glim_screen = nullptr, *camera_screen = nullptr, *swifty_screen = nullptr,
              *files_screen = nullptr, *tools_screen = nullptr, *gallery_screen = nullptr;
    lv_obj_t *home_screen = nullptr, *home_frame = nullptr;
    lv_indev_read_cb_t touch_read = nullptr;
    esp_lcd_touch_handle_t touch = nullptr;
    bool touch_pressed = false;
} shell;

lv_obj_t *app_screen(lv_obj_t **frame, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *f = lv_obj_create(screen);
    lv_obj_remove_style_all(f);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_size(f, w, h);
    lv_obj_remove_flag(f, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_bg_color(f, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    *frame = f;
    return screen;
}

// The AMOLED redesign: Glimdock starts in its dark (AMOLED) theme once; the
// person can still choose Light or Auto in Glimdock's settings afterwards.
void adopt_amoled_once()
{
    Preferences shell_prefs;
    if (!shell_prefs.begin(PREFS, false)) {
        return;
    }
    if (!shell_prefs.getBool("amoled", false)) {
        Preferences display;
        if (display.begin("display", false)) {
            display.putBool("light", false);
            display.putUChar("themeMode", 1);  // THEME_DARK
            display.end();
        }
        shell_prefs.putBool("amoled", true);
    }
    shell_prefs.end();
}

void load_prefs()
{
    Preferences p;
    if (p.begin(PREFS, true)) {
        shell.radius = std::clamp<int>(p.getUChar("radius", DEFAULT_RADIUS), 0, MAX_RADIUS);
        shell.current = ShellApp(p.getUChar("app", uint8_t(ShellApp::Launcher)) % SHELL_APPS);
        shell.brightness = std::clamp<int>(p.getUChar("bright", 80), 5, 100);
        p.end();
    }
}

void save_app(ShellApp app)
{
    Preferences p;
    if (p.begin(PREFS, false)) {
        if (p.getUChar("app", 255) != uint8_t(app)) {
            p.putUChar("app", uint8_t(app));
        }
        p.end();
    }
}

// ESP-Iris's restart also resets the S31's digital peripherals (a plain
// esp_restart leaves the USB engine running into the next boot: no USB after it).
void restart_board()
{
    esp_iris_mark_planned_restart();
    esp_iris_restart_now();
    esp_restart();  // only if Iris's restart was already taken
}

void save_radius(int radius)
{
    Preferences p;
    if (p.begin(PREFS, false)) {
        p.putUChar("radius", uint8_t(std::clamp(radius, 0, MAX_RADIUS)));
        p.end();
    }
    restart_board();  // every application lays out from the radius at start-up
}

void set_brightness(int percent, bool save)
{
    shell.brightness = std::clamp(percent, 5, 100);
    bsp_display_brightness_set(shell.brightness);
    if (!save) {
        return;
    }
    Preferences p;
    if (p.begin(PREFS, false)) {
        p.putUChar("bright", uint8_t(shell.brightness));
        p.end();
    }
}

void start_apps();

// A brightness change for a moment (screen light, screen flash); -1 restores
// the person's setting. It is a panel command on the frames' SPI link, so it
// waits for the frame being sent.
void panel_brightness(int percent)
{
    frame_sync_wait_idle(100);
    bsp_display_brightness_set(percent < 0 ? shell.brightness : std::clamp(percent, 5, 100));
}

void start_wifi()
{
    app_config_wifi_t wifi;
    esp_err_t err = app_config_load_wifi(&wifi);
    if (err == ESP_OK) {
        glimdock_app_set_platform_wifi(wifi.ssid, wifi.password);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(wifi.ssid, wifi.password);
        // Clocks in the launcher, Glimdock and Home need time before any app is
        // configured; SNTP waits for the network itself.
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        time_zone_start();  // automatic: looked up once the network is up
        ESP_LOGI(TAG, "joining the Vibe Mode Wi-Fi network");
    } else {
        WiFi.init();
        ESP_LOGW(TAG, "no Vibe Mode Wi-Fi (%s); set it in Vibe Mode", esp_err_to_name(err));
    }
    mbedtls_platform_zeroize(&wifi, sizeof(wifi));
}

// A real control: it (or the parent its clicks bubble to) acts on a click,
// a long press or a value change. Scroll areas and plain cards do not, and
// keyboard keys tick on their own (USER_1).
bool is_control(lv_obj_t *o)
{
    for (int depth = 0; o && depth < 4; ++depth) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1)) {
            return false;
        }
        const uint32_t n = lv_obj_get_event_count(o);
        for (uint32_t i = 0; i < n; ++i) {
            const lv_event_dsc_t *e = lv_obj_get_event_dsc(o, i);
            const uint32_t f = e ? (e->filter & ~uint32_t(LV_EVENT_PREPROCESS)) : 0;
            if (f == LV_EVENT_CLICKED || f == LV_EVENT_SHORT_CLICKED || f == LV_EVENT_SINGLE_CLICKED ||
                    f == LV_EVENT_VALUE_CHANGED || f == LV_EVENT_LONG_PRESSED) {
                return true;
            }
        }
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE)) {
            return false;
        }
        o = lv_obj_get_parent(o);
    }
    return false;
}

// RPC 48: a scripted touch, to test gestures (a pinch takes two fingers)
// without a finger on the glass. Keyframes of 11 bytes, little endian: the
// time from the start in ms (u16), the fingers down (0 to 2), then two points
// (x, y: i16). Between keyframes with the same fingers the points move in a
// straight line. The panel is ignored until the last keyframe.
struct TouchKey {
    int ms, n;
    lv_point_t p[2];
};
struct {
    TouchKey keys[32];
    int count = 0;
    std::atomic<bool> pending{false}, running{false};
    int64_t start = 0;
    lv_point_t last = {};
} script;

esp_err_t touch_script_rpc(const esp_iris_rpc_request_t *request, uint8_t *, size_t, size_t *size, void *)
{
    *size = 0;
    const size_t n = request->payload_size / 11;
    if (!n || n > std::size(script.keys) || request->payload_size % 11) {
        return ESP_ERR_INVALID_ARG;
    }
    if (script.pending || script.running) {
        return ESP_ERR_INVALID_STATE;
    }
    const auto u16 = [](const uint8_t *b) {
        return int(b[0] | b[1] << 8);
    };
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *b = request->payload + i * 11;
        TouchKey &k = script.keys[i];
        k.ms = u16(b), k.n = std::min<int>(b[2], 2);
        for (int f = 0; f < 2; ++f) {
            k.p[f] = {int16_t(u16(b + 3 + f * 4)), int16_t(u16(b + 5 + f * 4))};
        }
    }
    script.count = int(n);
    script.pending = true;  // the touch read starts it
    return ESP_OK;
}

// Replaces what the panel read with the script's fingers at this moment.
void play_touch_script(lv_indev_data_t *data)
{
    if (script.pending) {
        script.pending = false;
        script.running = true;
        script.start = esp_timer_get_time();
    }
    const int ms = int((esp_timer_get_time() - script.start) / 1000);
    int k = 0;
    while (k + 1 < script.count && script.keys[k + 1].ms <= ms) {
        ++k;
    }
    const TouchKey &a = script.keys[k];
    lv_point_t p[2] = {a.p[0], a.p[1]};
    if (k + 1 < script.count && script.keys[k + 1].n == a.n) {
        const TouchKey &b = script.keys[k + 1];
        const float t = float(ms - a.ms) / float(std::max(1, b.ms - a.ms));
        for (int f = 0; f < 2; ++f) {
            p[f] = {int32_t(lroundf(a.p[f].x + (b.p[f].x - a.p[f].x) * t)), int32_t(lroundf(a.p[f].y + (b.p[f].y - a.p[f].y) * t))};
        }
    }
    if (k + 1 >= script.count) {
        script.running = false;  // the last keyframe: back to the panel
    }
    if (a.n) {
        script.last = p[0];
    }
    data->point = script.last;
    data->state = a.n ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    mui::note_touches(a.n, p);
}

void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    shell.touch_read(indev, data);
    // Every finger, for pinch: the adapter passes LVGL the first only, and
    // reading the touch's last report again does not consume it.
    if (shell.touch) {
        esp_lcd_touch_point_data_t t[2] = {};
        uint8_t n = 0;
        lv_point_t p[2];
        if (esp_lcd_touch_get_data(shell.touch, t, &n, 2) != ESP_OK || data->state != LV_INDEV_STATE_PRESSED) {
            n = 0;
        }
        for (int i = 0; i < n; ++i) {
            p[i] = {int32_t(t[i].x), int32_t(t[i].y)};
        }
        mui::note_touches(n, p);
    }
    if (script.pending || script.running) {
        play_touch_script(data);
    }
    const bool pressed = data->state == LV_INDEV_STATE_PRESSED;
    if (pressed != shell.touch_pressed) {
        shell.touch_pressed = pressed;
        if (shell.current == ShellApp::Glimdock) {
            glimdock_app_note_touch(pressed);
        }
    }
}

// The fuel gauge (BQ27220), read-only: StateOfCharge (0x2C, %) and Current
// (0x0C, mA, positive while charging). Not bsp_battery_init(): that rewrites
// the gauge's profile, which Vibe Mode keeps its own of (see board_mosaico.cpp).
struct {
    int pct = -1;
    bool charging = false;
    int64_t next = 0;
    i2c_master_dev_handle_t dev = nullptr;
    bool failed = false;
} battery;

void read_battery()
{
    const int64_t now = esp_timer_get_time();
    if (battery.failed || now < battery.next) {
        return;
    }
    battery.next = now + 10 * 1000000LL;
    if (!battery.dev) {
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        i2c_device_config_t device = {};
        device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device.device_address = BSP_BATTERY_I2C_ADDR;
        device.scl_speed_hz = 100000;
        if (!bus || i2c_master_bus_add_device(bus, &device, &battery.dev) != ESP_OK) {
            battery.failed = true;
            return;
        }
    }
    auto word = [](uint8_t command, int *out) {
        uint8_t v[2] = {};
        if (i2c_master_transmit_receive(battery.dev, &command, 1, v, 2, 50) != ESP_OK) {
            return false;
        }
        *out = int16_t(v[0] | v[1] << 8);
        return true;
    };
    int soc = 0, current = 0;
    if (word(0x2C, &soc) && soc >= 0 && soc <= 100) {
        battery.pct = soc;
    }
    if (word(0x0C, &current)) {
        battery.charging = current > 15;
    }
}

void refresh_launcher(lv_timer_t *)
{
    if (shell.current != ShellApp::Launcher) {
        return;
    }
    uint8_t zone = 0;
    Preferences p;
    if (p.begin("display", true)) {  // Glimdock's clock zone
        zone = p.getUChar("clockZone", 0) % DISPLAY_CLOCK_ZONE_COUNT;
        p.end();
    }
    tm local{};
    char clock[8] = "--:--", date[24] = "", ip[24] = "", ssid[40] = "", versions[48];
    if (displayClockTm(time(nullptr), zone, local)) {
        static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
                                            };
        snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        snprintf(date, sizeof(date), "%s %d %s", days[local.tm_wday % 7], local.tm_mday, months[local.tm_mon % 12]);
    }
    const bool connected = WiFi.status() == WL_CONNECTED;
    app_config_wifi_t probe;
    const bool configured = app_config_load_wifi(&probe) == ESP_OK;
    if (configured) {
        strlcpy(ssid, probe.ssid, sizeof(ssid));
    }
    mbedtls_platform_zeroize(&probe, sizeof(probe));
    if (connected) {
        strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
    }
    snprintf(versions, sizeof(versions), "Firmware %s", esp_app_get_description()->version);
    glimdock_summary_t glim;
    glimdock_app_summary(&glim);
    home_summary_t home;
    home_app_summary(&home);
    camera_summary_t camera;
    camera_app_summary(&camera);
    swifty_summary_t swifty;
    swifty_app_summary(&swifty);
    files_summary_t files;
    files_app_summary(&files);
    read_battery();
    char zone_text[64];
    time_zone_text(zone_text, sizeof(zone_text));
    WebSettingsInfo web;
    web_settings_info(&web);
    launcher_ui_update({clock, date, connected, configured, ssid, ip, versions, battery.pct, battery.charging, zone_text, camera_app_auto_facing(),
                        web.running, web.url, web.code, web.note},
                       glim, home, camera, swifty, files);
}

void go_launcher_async(void *)
{
    shell_show(ShellApp::Launcher);
}

// ESP-Iris RPC 0x4744/30: open an app (payload: 0 launcher, 1 Glimdock, 2 Home,
// 3 Camera), for development and testing from the Mac.
void open_async(void *app)
{
    shell_show(ShellApp(uintptr_t(app)));
}
esp_err_t open_rpc(const esp_iris_rpc_request_t *request, uint8_t *, size_t, size_t *size, void *)
{
    *size = 0;
    if (request->payload_size < 1 || request->payload[0] >= SHELL_APPS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    // Tools may name its tab too (a second byte: 0 torch ... 4 the Interaction
    // board), and Glimdock its page (0 Systems, 1 Clock, ... 10 Agents).
    if (request->payload_size >= 2 && request->payload[0] == uint8_t(ShellApp::Tools)) {
        tools_app_select(request->payload[1]);
    }
    if (request->payload_size >= 2 && request->payload[0] == uint8_t(ShellApp::Glimdock)) {
        glimdock_app_select(request->payload[1]);
    }
    lv_async_call(open_async, reinterpret_cast<void *>(uintptr_t(request->payload[0])));
    bsp_display_unlock();
    return ESP_OK;
}

void ai_key_clicked(void *, void *)
{
    haptics_pulse(HapticPulse::Key);
    if (bsp_display_lock(1000)) {
        lv_async_call(go_launcher_async, nullptr);
        bsp_display_unlock();
    }
}

// ---- board configuration: Glimdock and Home sections ----------------------------------
// tools/mosaico-config.py loads one file for Swifty, Glimdock and Home;
// Swifty's config RPC hands these sections over.
const char *glimdock_apply(const char *json, size_t length)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, length) || !doc.is<JsonObject>()) {
        return "expected an object";
    }
    static const char *const KEYS[] = {"ssid", "password", "endpoint", "token", "setup_token", "ca"};
    for (JsonPair kv : doc.as<JsonObject>()) {
        bool known = false;
        for (const char *k : KEYS) {
            known |= !strcmp(k, kv.key().c_str());
        }
        if (!known) {
            return "unknown key (use ssid, password, endpoint, token, setup_token, ca)";
        }
        if (!kv.value().is<const char *>()) {
            return "values must be strings";
        }
    }
    auto field = [&](const char *key) {
        return doc[key].is<const char *>() ? doc[key].as<const char *>() : nullptr;
    };
    const glimdock_connection_t c = {field("ssid"), field("password"), field("endpoint"), field("token"), field("setup_token"), field("ca")};
    const char *error = glimdock_app_store_connection(&c);
    return error ? error : "restart";  // network.cpp reads its settings at start
}

void glimdock_status(char *out, size_t capacity)
{
    glimdock_connection_info_t info;
    glimdock_app_connection_info(&info);
    JsonDocument doc;
    doc["ssid"] = info.ssid;
    doc["endpoint"] = info.endpoint;
    doc["password"] = info.password;
    doc["token"] = info.token;
    doc["setup_token"] = info.setup_token;
    doc["ca"] = info.ca;
    serializeJson(doc, out, capacity);
}

struct PairRequest {
    char hub[160], code[24];
};
std::atomic<PairRequest *> pending_pair{nullptr};

void pair_async(void *arg)
{
    auto *r = static_cast<PairRequest *>(arg);
    home_app_pair(r->hub, r->code);
    mbedtls_platform_zeroize(r, sizeof(*r));
    delete r;
}

const char *home_apply(const char *json, size_t length)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, length) || !doc.is<JsonObject>()) {
        return "expected an object";
    }
    for (JsonPair kv : doc.as<JsonObject>())
        if (strcmp(kv.key().c_str(), "hub") && strcmp(kv.key().c_str(), "pairing_code")) {
            return "unknown key (use hub, pairing_code)";
        }
    const char *hub = doc["hub"] | "", *code = doc["pairing_code"] | "";
    if (!*hub || !*code) {
        return "hub and pairing_code are both needed (make a code in Swifty Home: Displays, Pair)";
    }
    auto *r = new PairRequest{};
    strlcpy(r->hub, hub, sizeof(r->hub));
    strlcpy(r->code, code, sizeof(r->code));
    // Handed to the LVGL task through a slot, without the display lock (an
    // ESP-Iris handler could not always get it).
    ESP_LOGI(TAG, "Home pairing queued");
    if (PairRequest *old = pending_pair.exchange(r)) {
        mbedtls_platform_zeroize(old, sizeof(*old));
        delete old;
    }
    return nullptr;  // pairing runs on; the tool follows it in the status
}

void home_status(char *out, size_t capacity)
{
    home_pairing_info_t info;
    home_app_pairing_info(&info);
    JsonDocument doc;
    doc["paired"] = info.paired;
    doc["hub"] = info.hub;
    doc["name"] = info.name;
    doc["pairing"] = info.note;
    serializeJson(doc, out, capacity);
}

// Held: open Swifty and listen until released.
std::atomic<bool> ai_holding{false};
void swifty_listen_async(void *on)
{
    if (on) {
        const int64_t t0 = esp_timer_get_time();
        if (shell.current != ShellApp::Swifty) {
            shell_show(ShellApp::Swifty);
        }
        const int64_t t1 = esp_timer_get_time();
        swifty_app_talk(true);
        ESP_LOGI(TAG, "AI key: Swifty shown in %lld ms, listening %lld ms later", (long long)((t1 - t0) / 1000),
                 (long long)((esp_timer_get_time() - t1) / 1000));
    } else {
        swifty_app_talk(false);
    }
}

void ai_key_double(void *, void *)
{
    haptics_pulse(HapticPulse::Key);
    if (bsp_display_lock(1000)) {
        lv_async_call(open_async, reinterpret_cast<void *>(uintptr_t(ShellApp::Swifty)));
        bsp_display_unlock();
    }
}

// The key is down: a click goes to the launcher, a hold talks. Start the
// speech connection now, so a hold finds it (nearly) ready.
void ai_key_down(void *, void *)
{
    swifty_app_prewarm(true);
}

void ai_key_hold(void *, void *)
{
    haptics_pulse(HapticPulse::Key);
    ai_holding = true;
    if (bsp_display_lock(1000)) {
        lv_async_call(swifty_listen_async, reinterpret_cast<void *>(1));
        bsp_display_unlock();
    }
}

void ai_key_up(void *, void *)
{
    if (!ai_holding.exchange(false)) {
        return;
    }
    if (bsp_display_lock(1000)) {
        lv_async_call(swifty_listen_async, nullptr);
        bsp_display_unlock();
    }
}

void start_ai_key()
{
    button_handle_t buttons[BSP_BUTTON_NUM] = {};
    int count = 0;
    if (bsp_iot_button_create(buttons, &count, BSP_BUTTON_NUM) != ESP_OK || count < 1) {
        ESP_LOGW(TAG, "AI key unavailable; use the launcher tiles only");
        return;
    }
    // Click: the launcher (as before). Double click: Swifty. Hold: talk to Swifty.
    // The hold starts listening after 0.45 s (the button default, 1.5 s, felt
    // as if nothing happened).
    // Hold to talk: 300 ms tells a hold from a click (under ~200 ms) and starts
    // listening a third sooner than the 450 ms it was.
    iot_button_set_param(buttons[BSP_BUTTON_AI], BUTTON_LONG_PRESS_TIME_MS, reinterpret_cast<void *>(uintptr_t(300)));
    iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_SINGLE_CLICK, nullptr, ai_key_clicked, nullptr);
    iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_DOUBLE_CLICK, nullptr, ai_key_double, nullptr);
    iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_PRESS_DOWN, nullptr, ai_key_down, nullptr);
    iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_LONG_PRESS_START, nullptr, ai_key_hold, nullptr);
    iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_PRESS_UP, nullptr, ai_key_up, nullptr);
}
}  // namespace

// The camera apps and webcam mode: no speech connection kept meanwhile.
static bool camera_screen()
{
    return shell.current == ShellApp::Camera || shell.current == ShellApp::Gallery || usb_webcam_owns_usb();
}

void shell_show(ShellApp app)
{
    const ShellApp previous = shell.current;
    launcher_ui_calibration(false, shell.radius);
    glimdock_app_set_active(false);
    home_app_set_active(false);
    camera_app_set_active(false);
    swifty_app_set_active(false);
    files_app_set_active(false);
    tools_app_set_active(false);
    gallery_app_set_active(false);
    shell.current = app;
    const int64_t t0 = esp_timer_get_time();
    save_app(app);
    const int64_t t1 = esp_timer_get_time();
    switch (app) {
    case ShellApp::Glimdock:
        lv_screen_load(shell.glim_screen);
        glimdock_app_open(shell.glim_screen);
        break;
    case ShellApp::Camera:
        lv_screen_load(shell.camera_screen);
        camera_app_open(shell.camera_screen);
        break;
    case ShellApp::Swifty:
        lv_screen_load(shell.swifty_screen);
        swifty_app_open(shell.swifty_screen);
        break;
    case ShellApp::Files:
        lv_screen_load(shell.files_screen);
        files_app_open(shell.files_screen);
        break;
    case ShellApp::Tools:
        lv_screen_load(shell.tools_screen);
        tools_app_open(shell.tools_screen);
        break;
    case ShellApp::Gallery:
        camera_app_start(shell.camera_screen);  // its photos: memory and the NAS list
        lv_screen_load(shell.gallery_screen);
        gallery_app_open(shell.gallery_screen);
        break;
    case ShellApp::Home:
        lv_screen_load(shell.home_screen);
        home_app_open(shell.home_frame);
        home_app_set_active(true);
        break;
    default:
        lv_screen_load(shell.launcher);
        refresh_launcher(nullptr);
        launcher_ui_entered(previous);
        break;
    }
    static const char *const NAMES[SHELL_APPS] = {"launcher", "Glimdock", "Home", "Camera", "Swifty", "Files", "Tools", "Gallery"};
    ESP_LOGI(TAG, "showing %s (saving it %lld ms, opening %lld ms)", NAMES[int(app) % SHELL_APPS], (long long)((t1 - t0) / 1000),
             (long long)((esp_timer_get_time() - t1) / 1000));
    swifty_app_prewarm(!camera_screen());  // at once: a press right after leaving the camera finds it ready sooner
}

namespace {
void log_memory(const char *what)
{
    ESP_LOGI(TAG, "%s; internal %u free, DMA %u (largest %u)", what,
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_DMA)), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
}

// ---- Swifty's tools: what each app can tell and do ---------------------------------------------
namespace agent_tools {
// Runs f on the LVGL task (under its lock, from Swifty's task) and waits.
template <typename F> bool on_ui(F f, int timeout_ms = 3000)
{
    if (!bsp_display_lock(timeout_ms)) {
        return false;
    }
    f();
    bsp_display_unlock();
    return true;
}

JsonDocument args(const char *json)
{
    JsonDocument d;
    if (json && *json) {
        deserializeJson(d, json);
    }
    return d;
}

const char *const APP_NAMES[SHELL_APPS] = {"launcher", "glimdock", "home", "camera", "swifty", "files", "tools", "gallery"};

void open_app(const char *a, char *out, size_t cap)
{
    const JsonDocument d = args(a);
    const std::string name = d["app"] | "", tool = d["tool"] | "";
    static const char *const TOOLS[] = {"torch", "level", "compass", "calculator", "interact"};
    int tool_index = -1;
    for (int t = 0; t < 5; ++t)
        if (tool == TOOLS[t]) {
            tool_index = t;
        }
    for (int i = 0; i < SHELL_APPS; ++i)
        if (name == APP_NAMES[i]) {
            on_ui([i, tool_index] {
                if (i == int(ShellApp::Tools) && tool_index >= 0) tools_app_select(tool_index);
                lv_async_call(open_async, reinterpret_cast<void *>(uintptr_t(i)));
            });
            snprintf(out, cap, "Opened %s.", APP_NAMES[i]);
            return;
        }
    snprintf(out, cap, "No app \"%s\". Apps: launcher, glimdock, home, camera, swifty, files, tools, gallery.", name.c_str());
}

void device_status(const char *, char *out, size_t cap)
{
    char zone[64];
    time_zone_text(zone, sizeof(zone));
    const bool wifi = WiFi.status() == WL_CONNECTED;
    camera_summary_t cam{};
    camera_app_summary(&cam);
    const int64_t up = esp_timer_get_time() / 1000000;
    snprintf(out, cap,
             "Board: ESP-Mosaico (ESP32-S31), firmware %s.\nBattery: %s%s.\nWi-Fi: %s%s%s (signal %d dBm).\n"
             "Screen brightness %d%%, haptics %s, auto-rotate %s.\nTime zone: %s.\nShowing: %s.\n"
             "Camera: %d photo(s)%s.\nMemory free: %u KB internal, %u KB PSRAM. Up %d h %d min.",
             esp_app_get_description()->version,
             battery.pct < 0 ? "unknown" : (std::to_string(battery.pct) + "%").c_str(), battery.charging ? ", charging" : "",
             wifi ? WiFi.SSID().c_str() : "not connected", wifi ? ", " : "", wifi ? WiFi.localIP().toString().c_str() : "",
             wifi ? int(WiFi.RSSI()) : 0, shell.brightness, haptics_enabled() ? "on" : "off",
             orientation_enabled() ? "on" : "off", zone, APP_NAMES[int(shell.current)], cam.photos,
             cam.present ? "" : ", no camera seen yet", unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), int(up / 3600), int(up / 60 % 60));
}

void set_brightness_tool(const char *a, char *out, size_t cap)
{
    const int pct = std::clamp(int(args(a)["percent"] | -1), -1, 100);
    if (pct < 0) {
        snprintf(out, cap, "Give percent, 5 to 100.");
        return;
    }
    on_ui([pct] { set_brightness(pct, true); });
    snprintf(out, cap, "Brightness %d%%.", shell.brightness);
}

void glimdock_tool(const char *a, char *out, size_t cap)
{
    std::string topic = args(a)["topic"] | "";
    if (topic == "all") {
        topic.clear();
    }
    glimdock_app_report(topic.c_str(), out, cap);
}

void home_state(const char *, char *out, size_t cap)
{
    if (!on_ui([&] { home_app_describe(out, cap); })) snprintf(out, cap, "The screen is busy; try again.");
}

void home_control(const char *a, char *out, size_t cap)
{
    const JsonDocument d = args(a);
    const std::string action = d["action"] | "", target = d["target"] | "";
    std::string value;
    if (d["value"].is<const char *>()) {
        value = d["value"].as<const char *>();
    } else if (!d["value"].isNull()) {
        value = std::to_string(d["value"].as<int>());
    }
    if (!on_ui([&] { home_app_command(action.c_str(), target.c_str(), value.c_str(), out, cap); }))
        snprintf(out, cap, "The screen is busy; try again.");
}

void flashlight(const char *a, char *out, size_t cap)
{
    const bool want = args(a)["on"] | true;
    bool ok = false;
    on_ui([&] {
        camera_app_start(shell.camera_screen);
        ok = camera_app_torch(want);
    });
    snprintf(out, cap, ok ? "Flashlight %s." : "The flashlight is not available (no camera module).", want ? "on" : "off");
}

void list_files(const char *a, char *out, size_t cap)
{
    const std::string path = args(a)["path"] | "";
    files_app_tool_list(path.c_str(), out, cap);
}

void read_file(const char *a, char *out, size_t cap)
{
    const std::string path = args(a)["path"] | "";
    files_app_tool_read(path.c_str(), out, cap);
}

void level(const char *, char *out, size_t cap)
{
    float x, y, z;
    if (!orientation_gravity(&x, &y, &z)) {
        snprintf(out, cap, "No motion sensor.");
        return;
    }
    const float g = sqrtf(x * x + y * y + z * z);
    if (g < 0.2f) {
        snprintf(out, cap, "No reading.");
        return;
    }
    // Tilt of the glass from flat, and the turn of the screen from upright.
    const float flat = acosf(std::clamp(fabsf(z) / g, 0.0f, 1.0f)) * 57.2958f;
    const float turn = atan2f(x, y) * 57.2958f;
    snprintf(out, cap, "The board is tilted %.1f degrees from lying flat; held up, it is turned %.1f degrees from upright.",
             double(flat), double(turn));
}

void time_zone_tool(const char *a, char *out, size_t cap)
{
    const std::string want = args(a)["zone"] | "";
    if (want == "automatic") {
        time_zone_set(-1);
    } else {
        int z = -1;
        for (int i = 0; i < time_zone_count(); ++i)
            if (!strcasecmp(want.c_str(), time_zone_name(i))) {
                z = i;
            }
        if (z < 0) {
            std::string names = "automatic";
            for (int i = 0; i < time_zone_count(); ++i) {
                names += std::string(", ") + time_zone_name(i);
            }
            snprintf(out, cap, "The clocks know: %s.", names.c_str());
            return;
        }
        time_zone_set(z);
    }
    char text[64];
    time_zone_text(text, sizeof(text));
    snprintf(out, cap, "Time zone: %s.", text);
}

// The Interaction sub-board (Tools): these wait on its worker, not the UI.
void board_state(const char *, char *out, size_t cap)
{
    tools_board_report(out, cap);
}

void board_lights(const char *a, char *out, size_t cap)
{
    const JsonDocument d = args(a);
    const std::string mode = d["mode"] | "", colour = d["colour"] | (d["color"] | "");
    tools_board_lights(mode.c_str(), colour.c_str(), out, cap);
}

void tv_remote(const char *a, char *out, size_t cap)
{
    const JsonDocument d = args(a);
    const std::string key = d["key"] | "";
    tools_board_ir(key.c_str(), d["address"] | -1, d["command"] | -1, out, cap);
}

void register_all()
{
    swifty_app_add_tool("open_app", "Show one of the board's apps on the screen.",
                        R"({"type":"object","properties":{"app":{"type":"string","enum":["launcher","glimdock","home","camera","swifty","files","tools","gallery"]},"tool":{"type":"string","enum":["torch","level","compass","calculator","interact"],"description":"for tools: which tool; interact is the Interaction board"}},"required":["app"]})",
                        open_app);
    swifty_app_add_tool("device_status",
                        "The board itself: battery and charging, Wi-Fi, brightness, time zone, the app shown, photos, memory, uptime.",
                        R"({"type":"object","properties":{},"required":[]})", device_status);
    swifty_app_add_tool("set_brightness", "Set the screen brightness.",
                        R"({"type":"object","properties":{"percent":{"type":"integer","minimum":5,"maximum":100}},"required":["percent"]})",
                        set_brightness_tool);
    swifty_app_add_tool("glimdock",
                        "The homelab (Glimdock): systems and their health, CPU, memory, temperatures, guests, storage, disks, "
                        "alerts, the 3D printer, and Kordlet agent sessions and cards (what needs the person).",
                        R"({"type":"object","properties":{"topic":{"type":"string","enum":["all","fleet","system","agents"],"description":"fleet: every system; system: the one shown in detail; agents: Kordlet."}},"required":[]})",
                        glimdock_tool);
    swifty_app_add_tool("home_state", "Every room's lights (on, brightness), the speaker, TV, plugs and air conditioning, from Swifty Home.",
                        R"({"type":"object","properties":{},"required":[]})", home_state);
    swifty_app_add_tool("home_control",
                        "Control the home. Actions: lights_on, lights_off, brightness (value 0-100), color (value: warm, cool, red, "
                        "orange, yellow, green, blue, purple, pink or a hue), scene (value: Movie, Evening, Lights off), volume "
                        "(value 0-100), play, pause, mute, unmute, tv_on, tv_off, plug_on, plug_off. Target: a room, a light or a "
                        "plug by name, or all (lights).",
                        R"({"type":"object","properties":{"action":{"type":"string"},"target":{"type":"string"},"value":{"type":"string"}},"required":["action"]})",
                        home_control);
    swifty_app_add_tool("flashlight", "Turn the camera module's light on or off.",
                        R"({"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]})", flashlight);
    swifty_app_add_tool("list_files",
                        "List a folder on the NAS shares or the board (Files). Empty path: the places. Paths start with a place, "
                        "e.g. Media/Videos or Photos.",
                        R"({"type":"object","properties":{"path":{"type":"string"}},"required":[]})", list_files);
    swifty_app_add_tool("read_file", "Read a text file from the NAS shares or the board (its start, a few KB).",
                        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})", read_file);
    swifty_app_add_tool("level", "How the board is tilted (the Tools app's level).", R"({"type":"object","properties":{},"required":[]})",
                        level);
    swifty_app_add_tool("interaction_board",
                        "The Interaction sub-board (plugs into either side): its two buttons, PIR motion (and when it was last "
                        "seen), the light level, and its LED lights' mode.",
                        R"({"type":"object","properties":{},"required":[]})", board_state);
    swifty_app_add_tool("board_lights",
                        "Set the Interaction board's six LED lights: off, react (buttons and motion in colour), rainbow, night "
                        "(warm light for 30 s on motion in the dark) or colour (a fixed colour).",
                        R"({"type":"object","properties":{"mode":{"type":"string","enum":["off","react","rainbow","night","colour"]},"colour":{"type":"string","description":"a name (red, orange, yellow, green, cyan, blue, purple, pink, white, warm) or #RRGGBB"}},"required":[]})",
                        board_lights);
    swifty_app_add_tool("tv_remote",
                        "Press a key on the LG TV with the Interaction board's infrared LED (the board must face the TV). Or "
                        "send any NEC code with address and command.",
                        R"({"type":"object","properties":{"key":{"type":"string","enum":["power","volume_up","volume_down","mute","input","ok","up","down","left","right","back","home","channel_up","channel_down"]},"address":{"type":"integer"},"command":{"type":"integer"}},"required":[]})",
                        tv_remote);
    swifty_app_add_tool("set_time_zone", "Set the clocks' time zone, or automatic (from the network's location).",
                        R"({"type":"object","properties":{"zone":{"type":"string","description":"automatic, UTC, Sofia, London, Berlin, New York, Los Angeles or Tokyo"}},"required":["zone"]})",
                        time_zone_tool);
}
}  // namespace agent_tools

// What webcam mode did to the rest of the board (RPC 45, read once ESP-Iris is
// back): the longest gaps in the LVGL loop (taps shorter than a gap can be
// missed) and CPU per task, every 10 s.
namespace webcam_diag {
struct {
    int64_t since = 0, last = 0, worst = 0, worst_at = 0;  // us
    int over100 = 0, over300 = 0;
    char cpu[3][200] = {};  // the latest three windows, oldest first
    int64_t cpu_at[3] = {};
} D;

void reset()
{
    char unused[8];
    camera_app_webcam_stats(unused, sizeof(unused), true);
    D.since = esp_timer_get_time();
    D.last = D.worst = D.worst_at = 0;
    D.over100 = D.over300 = 0;
    for (int i = 0; i < 3; ++i) {
        D.cpu[i][0] = 0, D.cpu_at[i] = 0;
    }
}

void cpu_window()  // the health task
{
    for (int i = 0; i < 2; ++i) {
        memcpy(D.cpu[i], D.cpu[i + 1], sizeof(D.cpu[i]));
        D.cpu_at[i] = D.cpu_at[i + 1];
    }
    perf_cpu_window(D.cpu[2], sizeof(D.cpu[2]));
    D.cpu_at[2] = esp_timer_get_time() - D.since;
}

esp_err_t rpc(const esp_iris_rpc_request_t *, uint8_t *out, size_t cap, size_t *size, void *)
{
    char *o = reinterpret_cast<char *>(out);
    int n = snprintf(o, cap, "LVGL loop: worst gap %lld ms at %lld s, gaps over 100 ms %d, over 300 ms %d; CPU %% of a core:",
                     (long long)(D.worst / 1000), (long long)(D.worst_at / 1000000), D.over100, D.over300);
    for (int i = 0; i < 3 && n > 0 && size_t(n) < cap; ++i)
        if (D.cpu[i][0]) {
            n += snprintf(o + n, cap - n, " [to %llds] %s;", (long long)(D.cpu_at[i] / 1000000), D.cpu[i]);
        }
    if (n > 0 && size_t(n) + 8 < cap) {
        n += snprintf(o + n, cap - n, " camera: ");
        if (n > 0 && size_t(n) < cap) {
            camera_app_webcam_stats(o + n, cap - n, false);
        }
        n = int(strnlen(o, cap));
    }
    *size = n < 0 ? 0 : std::min(cap, size_t(n));
    return ESP_OK;
}
}  // namespace webcam_diag

// Every 10 s: whether the host still has the USB link, memory, and the camera
// (USB dropped during long camera sessions; ESP-Iris's TCP link carries
// this log when USB is gone).
void health_task(void *)
{
    bool was_mounted = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        // Webcam mode: the port is the camera's TinyUSB (or between owners),
        // and a reconnect would restart the computer's setup of the camera.
        static bool webcam_was = false;
        if (usb_webcam_owns_usb()) {
            if (!webcam_was) {
                perf_cpu_window(nullptr, 0);    // the first window starts now
            } else {
                webcam_diag::cpu_window();
            }
            webcam_was = true;
            was_mounted = true;
            continue;
        }
        webcam_was = false;
        camera_summary_t cam = {};
        camera_app_summary(&cam);
        const bool mounted = tud_mounted();
        ESP_LOGI("health", "uptime %llds | usb %s%s | internal %u (largest %u) DMA %u (largest %u) PSRAM %u | camera %s",
                 (long long)(esp_timer_get_time() / 1000000), mounted ? "mounted" : "NOT mounted",
                 tud_suspended() ? ", suspended" : "", unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), cam.streaming ? "streaming" : "off");
        if (was_mounted && !mounted) {
            ESP_LOGW("health", "USB link lost");
        }
        was_mounted = mounted;
        // Not configured by the host 10 s after boot (and every 30 s after):
        // after a software restart the pull-up can stay on through boot, so the
        // host never sees the board leave and come back, and gives up on the
        // port (no USB until Vibe Mode). A soft disconnect makes it enumerate
        // afresh; with no cable it changes nothing, and a working link is left.
        static int unmounted = 0;
        if (mounted) {
            unmounted = 0;
        } else if (unmounted++ % 3 == 0) {
            ESP_LOGW("health", "USB not configured by the host: reconnecting");
            tud_disconnect();
            vTaskDelay(pdMS_TO_TICKS(600));
            tud_connect();
        }
    }
}

void start_apps()
{
    xTaskCreatePinnedToCoreWithCaps(health_task, "health", 3072, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    boot_stage("start Glimdock");
    glimdock_app_start(shell.glim_screen);
    ESP_LOGI(TAG, "Glimdock started; internal heap %u free",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    boot_stage("start Home");
    home_app_start(shell.home_frame);
    boot_stage("apps running");
    log_memory("Home started");
    // Display SPI transfers need 7.5 KB DMA blocks: report the first minute.
    lv_timer_t *watch = lv_timer_create([](lv_timer_t *) {
        log_memory("running");
    }, 15000, nullptr);
    lv_timer_set_repeat_count(watch, 4);
}
}  // namespace

// LVGL draws into one 48-line buffer in internal DMA memory. The adapter's
// default buffers are in PSRAM, so the SPI driver copied every 7.5 KB chunk
// into internal DMA memory and kept up to ten copies (~77 KB) queued; with
// both apps running that ran out and the display stalled. One buffer rather
// than two of 24 lines: sending a strip takes ~1 ms, while every strip costs
// LVGL a walk of the whole screen, so fewer, taller strips scroll faster in
// the same 46 KB. Lines are a multiple of 8, the QSPI rounding unit.
void use_internal_draw_buffers(lv_display_t *display)
{
    constexpr size_t LINES = 48, BYTES = PANEL * LINES * 2;
    void *buffer = heap_caps_aligned_alloc(64, BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buffer) {
        ESP_LOGW(TAG, "no internal DMA draw buffer; keeping the adapter's");
        return;
    }
    lv_display_set_buffers(display, buffer, nullptr, BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

// 3 s after boot: if the host has not configured USB, reconnect once (the
// health task keeps checking every 30 s after). After a software restart the
// host often never saw the board leave (4 of 6 restarts on the board).
// ESP-IDF's esp_crt_bundle_detach() also clears the one global bundle pointer,
// so an HTTPS connection closing while another verifies its certificate made
// that one fail as untrusted ("the speech host's certificate was rejected",
// often: the speech handshake takes seconds). Linked with
// --wrap=esp_crt_bundle_detach (CMakeLists): undo only this connection's setup.
extern "C" void __wrap_esp_crt_bundle_detach(mbedtls_ssl_config *conf)
{
    if (!conf) {
        return;
    }
    mbedtls_ssl_conf_verify(conf, nullptr, nullptr);
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY)
    mbedtls_ssl_conf_ca_cb(conf, nullptr, nullptr);
#else
    mbedtls_ssl_conf_ca_chain(conf, nullptr, nullptr);
#endif
}

// USB webcam mode (Tools > Webcam): ESP-Iris steps aside and the USB port is
// a camera until it is switched off (a restart also ends it).
namespace webcam_mode {
std::atomic<int> switching{-1};  // -1 idle; else the switch under way (1 on, 0 off)
// Test sessions (RPC 43): make 720p frames with no computer watching, and an
// optional fixed rotation (-1: the accelerometer's).
std::atomic<bool> bench{false};
std::atomic<int> bench_rotation{-1};

// The camera asks every frame: the size the computer chose (none while it has
// no stream open) and up from the accelerometer, auto-rotate on or off.
void view(int *width, int *height, int *rotation)
{
    if (!usb_webcam_streaming() && !bench) {
        *width = *height = 0;
        return;
    }
    usb_webcam_format(width, height);
    *rotation = bench_rotation >= 0 ? bench_rotation.load() : orientation_board_rotation();
}

void worker(void *arg)
{
    if (arg) {
        vTaskDelay(pdMS_TO_TICKS(500));  // let a reply over ESP-Iris go out first
        swifty_app_prewarm(false);  // the webcam needs the internal memory its task holds
        if (usb_webcam_start() == ESP_OK) {
            camera_app_set_frame_sink(usb_webcam_push, view);
        }
    } else {
        camera_app_set_frame_sink(nullptr, nullptr);
        usb_webcam_stop();
        // A test session's settings end with it (a fixed rotation outlived the
        // test and stopped the next sessions following the accelerometer).
        bench = false;
        bench_rotation = -1;
        camera_app_webcam_yuv(false);
    }
    switching = -1;
    vTaskDelete(nullptr);
}

void set(bool on);

// Every 20 ms on the LVGL task while the webcam is on: the gaps in the LVGL
// loop, and the way out when the cable is pulled (or the computer sleeps):
// the bus stays suspended, so after 4 s the webcam goes and ESP-Iris is back.
void watch(lv_timer_t *)
{
    const int64_t now = esp_timer_get_time();
    auto &D = webcam_diag::D;
    if (!usb_webcam_active()) {
        D.last = 0;
        return;
    }
    if (D.last) {
        const int64_t gap = now - D.last;
        if (gap > D.worst) {
            D.worst = gap, D.worst_at = now - D.since;
        }
        if (gap > 100000) {
            ++D.over100;
        }
        if (gap > 300000) {
            ++D.over300;
        }
    }
    D.last = now;
    static int64_t suspended_since = 0;
    if (!tud_suspended()) {
        suspended_since = 0;
    } else if (!suspended_since) {
        suspended_since = now;
    } else if (now - suspended_since > 4000000 && switching == -1) {
        ESP_LOGW(TAG, "webcam: USB suspended for 4 s (unplugged?); turning it off");
        suspended_since = 0;
        set(false);
    }
}

void set(bool on)  // the LVGL task
{
    if (switching != -1 || on == usb_webcam_active()) {
        return;
    }
    if (on) {
        static lv_timer_t *watcher = lv_timer_create(watch, 20, nullptr);
        (void)watcher;
        webcam_diag::reset();
        camera_app_start(shell.camera_screen);
    }
    switching = on;
    // A plain stack: stopping ESP-Iris may touch flash.
    xTaskCreatePinnedToCore(worker, "webcam-switch", 4096, on ? reinterpret_cast<void *>(1) : nullptr, 3, nullptr, 0);
}

// Test hook (RPC 43, payload: seconds): on, then off again by itself, since
// nothing can reach the board over USB or Wi-Fi while it is a webcam.
esp_timer_handle_t off_timer = nullptr;
esp_err_t test_rpc(const esp_iris_rpc_request_t *request, uint8_t *, size_t, size_t *size, void *)
{
    *size = 0;
    // Payload: seconds; then 1 to make frames with no computer watching; then a
    // rotation / 90 + 1 to fix it (0: the accelerometer's).
    const int seconds = request->payload_size ? request->payload[0] : 60;
    bench = request->payload_size >= 2 && request->payload[1] == 1;
    bench_rotation = request->payload_size >= 3 && request->payload[2] ? (request->payload[2] - 1) * 90 : -1;
    camera_app_webcam_yuv(request->payload_size >= 4 && request->payload[3] == 2);  // then 2: YUV420 to the encoder
    if (!off_timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void *) {
            if (!bsp_display_lock(1000)) {
                return;
            }
            lv_async_call([](void *) {
                set(false);
            }, nullptr);
            bsp_display_unlock();
        };
        args.name = "webcam-off";
        if (esp_timer_create(&args, &off_timer) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    esp_timer_stop(off_timer);
    if (seconds <= 0) {
        set(false);
        return ESP_OK;
    }
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_async_call([](void *) {
        set(true);
    }, nullptr);
    bsp_display_unlock();
    esp_timer_start_once(off_timer, uint64_t(seconds) * 1000000ULL);
    return ESP_OK;
}

bool status(char *text, size_t cap)
{
    const int s = switching;
    if (s >= 0) {
        strlcpy(text, s ? "Turning on\xE2\x80\xA6" : "Turning off\xE2\x80\xA6", cap);
        return s == 1;
    }
    if (!usb_webcam_active()) {
        strlcpy(text, "Tap the camera to start", cap);  // "Off" is above it already
        return false;
    }
    if (!usb_webcam_streaming()) {
        strlcpy(text, "On \xC2\xB7 choose \"ESP-Mosaico Camera\" on the computer", cap);
    } else {
        int w = 0, h = 0;
        usb_webcam_format(&w, &h);
        snprintf(text, cap, "Streaming %dx%d to the computer \xC2\xB7 %d fps", w, h, usb_webcam_fps());
    }
    return true;
}
}  // namespace webcam_mode

void usb_kick(void *)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (!tud_mounted()) {
        ESP_LOGW("health", "USB not configured after boot: reconnecting");
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(600));
        tud_connect();
    }
    vTaskDelete(nullptr);
}

extern "C" esp_err_t mosaico_shell_start(void)
{
    xTaskCreatePinnedToCore(usb_kick, "usb_kick", 2560, nullptr, 1, nullptr, 0);  // ends itself: a plain stack
    boot_stage("prefs");
    load_prefs();
    adopt_amoled_once();
    boot_stage("display start");
    // Glimdock's loop() and the TrueType rasterizer run on the LVGL task; the
    // adapter's default 8 KB stack overflowed (CPU lockup resets). Every app
    // opened peaks at ~6.5 KB (RPC 16); 32 KB kept 26 KB of internal RAM idle.
    bsp_display_config_t display_config = BSP_DISPLAY_DEFAULT_CONFIG();
    display_config.task_stack_size = 20 * 1024;
    display_config.buffer_height = 8;  // replaced by use_internal_draw_buffers
    // TE sync waits for a fresh panel refresh before every flush; with strip
    // buffers a full screen then took ~20 refreshes and painted top to bottom.
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT;
    lv_display_t *display = bsp_display_start_with_config(&display_config);
    if (!display) {
        ESP_LOGE(TAG, "display unavailable");
        return ESP_FAIL;
    }
    // Brightness is a panel command on the same SPI link as frames: send it
    // with the display lock held so it never overlaps a flush (an overlap
    // corrupted the SPI driver's in-flight count and hung start-up).
    boot_stage("brightness");
    if (bsp_display_lock(-1)) {
        bsp_display_brightness_set(shell.brightness);
        bsp_display_unlock();
    }
    boot_stage("haptics");
    haptics_init();
    mui::keyboard_set_haptic([] { haptics_pulse(HapticPulse::Key); });
    boot_stage("wifi");
    start_wifi();
    boot_stage("display lock");
    if (!bsp_display_lock(-1)) {
        return ESP_ERR_TIMEOUT;
    }
    boot_stage("draw buffers");
    if (!frame_sync_start(display)) {
        use_internal_draw_buffers(display);
    }
    perf_start(display);
    boot_stage("screens");
    // Camera: a full-panel viewfinder.
    shell.camera_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(shell.camera_screen);
    lv_obj_set_style_bg_color(shell.camera_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shell.camera_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(shell.camera_screen, LV_OBJ_FLAG_SCROLLABLE);
    // Swifty: a full-panel chat with its 3D figure.
    shell.swifty_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(shell.swifty_screen);
    lv_obj_set_style_bg_color(shell.swifty_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shell.swifty_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(shell.swifty_screen, LV_OBJ_FLAG_SCROLLABLE);
    {
        swifty_hooks_t hooks = {};
        hooks.exit = [] { shell_show(ShellApp::Launcher); };
        hooks.camera_ready = [] { camera_app_start(shell.camera_screen); };
        hooks.pick_photo = [] {
            shell_show(ShellApp::Camera);  // first: leaving an app clears a pending pick
            camera_app_pick([](int index) {
                shell_show(ShellApp::Swifty);
                swifty_app_attach_photo(index);
            });
        };
        swifty_app_init(&hooks);
        swifty_app_config_section("glimdock", glimdock_apply, glimdock_status);
        swifty_app_config_section("home", home_apply, home_status);
        swifty_app_config_section("files", files_app_config_apply, files_app_config_status);
        agent_tools::register_all();
    }
    // Files and Tools: plain black full-panel screens.
    for (lv_obj_t **scr : {
                &shell.files_screen, &shell.tools_screen, &shell.gallery_screen
            }) {
        *scr = lv_obj_create(nullptr);
        lv_obj_remove_style_all(*scr);
        lv_obj_set_style_bg_color(*scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(*scr, LV_OPA_COVER, 0);
        lv_obj_remove_flag(*scr, LV_OBJ_FLAG_SCROLLABLE);
    }
    {
        files_hooks_t fh = {};
        fh.exit = [] { shell_show(ShellApp::Launcher); };
        files_app_init(&fh);
        tools_hooks_t th = {};
        th.exit = [] { shell_show(ShellApp::Launcher); };
        th.torch = [](bool on) {
            camera_app_start(shell.camera_screen);
            return camera_app_torch(on);
        };
        th.gravity = orientation_gravity;
        th.to_ui = orientation_to_ui;
        th.haptic = [] { haptics_pulse(HapticPulse::Tap); };
        th.brightness = panel_brightness;
        th.webcam = webcam_mode::set;
        th.webcam_status = webcam_mode::status;
        tools_app_init(&th);
        camera_app_set_brightness_hook(panel_brightness);
        gallery_app_set_exit([] { shell_show(ShellApp::Launcher); });
        // The viewfinder's thumbnail opens the Gallery on the newest photo.
        camera_app_set_gallery_hook([] {
            gallery_app_show(-1);
            shell_show(ShellApp::Gallery);
        });
    }
    // Glimdock lays itself out over the whole panel (glim_ui.cpp).
    shell.glim_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(shell.glim_screen);
    lv_obj_set_style_bg_color(shell.glim_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shell.glim_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(shell.glim_screen, LV_OBJ_FLAG_SCROLLABLE);
    // Home: nearly the whole panel. Its pages keep their top row and side
    // padding clear of the corner curves; scrolling content passes them.
    const int home_x = int(lroundf(shell.radius * 0.15f)), home_y = int(lroundf(shell.radius * 0.1f));
    shell.home_screen =
        app_screen(&shell.home_frame, home_x, home_y, PANEL - 2 * home_x, PANEL - 2 * home_y, 0x000000);
    int widgets[2] = {-1, -1};
    {
        Preferences p;
        if (p.begin(PREFS, true)) {
            widgets[0] = p.getUChar("w0", 255) == 255 ? -1 : p.getUChar("w0", 0);
            widgets[1] = p.getUChar("w1", 255) == 255 ? -1 : p.getUChar("w1", 0);
            p.end();
        }
    }
    shell.launcher = launcher_ui_build({shell_show, save_radius, set_brightness, haptics_set_enabled, orientation_set_enabled,
                                        [](int left, int right)
    {
        Preferences p;
        if (p.begin(PREFS, false)) {
            p.putUChar("w0", uint8_t(left));
            p.putUChar("w1", uint8_t(right));
            p.end();
        }
    },
    [](int step)
    {
        time_zone_step(step);
        refresh_launcher(nullptr);  // the new choice and clock at once
    },
    camera_app_set_auto_facing,
    [](bool on)
    {
        if (on) {
            web_settings_start();
        } else {
            web_settings_stop("");
        }
        refresh_launcher(nullptr);  // the address and code, or why not, at once
    }},
    shell.radius, shell.brightness, haptics_enabled(), orientation_enabled(),
         swifty_app_portrait(168), widgets[0], widgets[1]);
    home_app_set_exit_handler([] { shell_show(ShellApp::Launcher); });
    web_settings_set_restart(restart_board);  // a save that applies after a restart
    lv_indev_t *touch = bsp_display_get_input_dev();
    if (touch) {
        shell.touch_read = lv_indev_get_read_cb(touch);
        if (bsp_touch_new(bsp_display_get_rotation(), &shell.touch) != ESP_OK) {  // the one the BSP made
            shell.touch = nullptr;
        }
        lv_indev_set_read_cb(touch, touch_read);
        // A pulse when a tap is a click on a control (LVGL sends no click for a
        // scroll), or a long press that does something: not for every touch.
        lv_indev_add_event_cb(touch, [](lv_event_t *e) {
            if (is_control(static_cast<lv_obj_t *>(lv_event_get_param(e)))) {
                haptics_pulse(HapticPulse::Tap);
            }
        }, LV_EVENT_CLICKED, nullptr);
        // A finger wobbles more than LVGL's 10 px (under 1 mm on this dense
        // panel) during a tap; past that the tap became a scroll and was lost,
        // which made Home's room tabs hard to hit.
        lv_indev_set_scroll_limit(touch, 20);
    }
    lv_timer_create(refresh_launcher, 1000, nullptr);
    // A speech connection kept ready on every screen, so a press of the AI key
    // starts recognising at once (a new one takes ~1.3 s: the server's
    // certificate chain is verified in software and it does not resume
    // sessions). Let go in the camera apps and webcam mode, which need the
    // internal memory its task holds. shell_show switches it at once too.
    lv_timer_create([](lv_timer_t *) {
        swifty_app_prewarm(!camera_screen());
    }, 20000, nullptr);
    lv_timer_create([](lv_timer_t *) {
        if (PairRequest *r = pending_pair.exchange(nullptr)) {
            ESP_LOGI(TAG, "pairing with Swifty Home at %s", r->hub);
            pair_async(r);
        }
    }, 200, nullptr);
    // Start on the launcher. Once the firmware has accepted itself
    // (ESP_MOSAICO_APP_ACCEPT_DELAY_MS) start both apps in the background, so
    // the launcher pages show live summaries, and reopen the last app: an app
    // fault during start-up then reboots this firmware instead of rolling back
    // to Vibe Mode.
    // Never the camera: a fault while it starts would then repeat on every
    // boot (a camera crash became a reboot loop).
    static ShellApp resume;
    resume = shell.current == ShellApp::Camera || shell.current == ShellApp::Tools ? ShellApp::Launcher : shell.current;
    shell_show(ShellApp::Launcher);
    lv_timer_t *later = lv_timer_create([](lv_timer_t *t) {
        lv_timer_delete(t);
        start_apps();
        if (resume != ShellApp::Launcher) {
            ESP_LOGI(TAG, "resuming the last app");
            shell_show(resume);
        }
    }, CONFIG_ESP_MOSAICO_APP_ACCEPT_DELAY_MS + 2500, nullptr);
    (void)later;
    boot_stage("unlock");
    bsp_display_unlock();
    boot_stage("ai key");
    start_ai_key();
    boot_stage("orientation");
    orientation_start();
    boot_stage("screenshot");
    esp_iris_rpc_register(0x4744, 30, open_rpc, nullptr);
    esp_iris_rpc_register(0x4744, 43, webcam_mode::test_rpc, nullptr);
    esp_iris_rpc_register(0x4744, 48, touch_script_rpc, nullptr);
    // 47: as the AI key, for timing the voice start from anywhere. Payload 1:
    // past its hold time (opens Swifty when needed, talks); 0: released; 2: a
    // real press (key down, the hold 300 ms later); 3: as a touch (activity).
    esp_iris_rpc_register(0x4744, 47, [](const esp_iris_rpc_request_t *request, uint8_t *, size_t, size_t *size, void *) {
        *size = 0;
        if (request->payload_size < 1) {
            return ESP_ERR_INVALID_ARG;
        }
        const uint8_t what = request->payload[0];
        if (what == 2) {
            ai_key_down(nullptr, nullptr);
        }
        if (!bsp_display_lock(1000)) {
            return ESP_ERR_TIMEOUT;
        }
        if (what == 3) {
            lv_display_trigger_activity(nullptr);
        } else if (what == 2) {
            lv_timer_t *t = lv_timer_create([](lv_timer_t *) {
                swifty_listen_async(reinterpret_cast<void *>(1));
            }, 300, nullptr);
            lv_timer_set_repeat_count(t, 1);
        } else {
            lv_async_call(swifty_listen_async, what ? reinterpret_cast<void *>(1) : nullptr);
        }
        bsp_display_unlock();
        return ESP_OK;
    }, nullptr);
    // 45: the LVGL loop and CPU per task during the last webcam session.
    esp_iris_rpc_register(0x4744, 45, webcam_diag::rpc, nullptr);
    // 46: the last webcam JPEG, payload a 32-bit offset (little-endian); 0 bytes at its end.
    esp_iris_rpc_register(0x4744, 46, [](const esp_iris_rpc_request_t *request, uint8_t *out, size_t cap, size_t *size, void *) {
        uint32_t offset = 0;
        if (request->payload_size >= 4) {
            memcpy(&offset, request->payload, 4);
        }
        *size = usb_webcam_last_frame(offset, out, std::min<size_t>(cap, 960));
        return ESP_OK;
    }, nullptr);
    // 44: what the last webcam session saw (read once ESP-Iris is back).
    esp_iris_rpc_register(0x4744, 44, [](const esp_iris_rpc_request_t *, uint8_t *out, size_t cap, size_t *size, void *) {
        usb_webcam_report(reinterpret_cast<char *>(out), cap);
        *size = strnlen(reinterpret_cast<char *>(out), cap);
        return ESP_OK;
    }, nullptr);
    shell_screenshot_register();
    ESP_LOGI(TAG, "ready; corner radius %d px", shell.radius);
    return ESP_OK;
}
