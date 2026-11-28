// SPDX-License-Identifier: Apache-2.0
// Host preview: 480x480 RGB565 captures of the launcher, Glimdock (the Mosaico
// UI over the firmware's demo data) and Swifty Home. Writes PPM files; render.py
// converts them and overlays the panel's rounded corners. With
// MOSAICO_PREVIEW_RECORD=1 it also records animations frame by frame (into a
// folder per clip) for host/media.py's GIFs and video.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "model.h"
#include "board.h"
#include "fixture.h"
#include "glim_ui.h"
#include "glimdock_app.h"
#include "launcher_ui.h"
#include "wifi_settings.h"
#include "tools_app.h"
#include "vpn_app.h"
#include "games_app.h"
#include "shortcuts_internal.h"
// games_ui.cpp: the preview's synchronous frames and screens
extern "C" void games_app_preview_frame(void);
extern "C" void games_app_preview_mode(int mode);
extern "C" void games_app_preview_best(int game, uint32_t score);
#include "swifty_figure.h"
#include "mui.h"
#include "home_app.h"
#include "home_hub.h"
#include <thread>
#include "lvgl.h"
#include "model.h"

namespace home {
void home_preview_open(int);
void home_preview_theme(int);
}
extern std::string home_fixture_dir;

uint32_t nativeClock = 1000;
uint32_t millis()
{
    return nativeClock;
}
Snapshot *publishedSnapshot = nullptr;
SemaphoreHandle_t dataMutex = nullptr;
NetworkState networkState;
volatile bool requestSetup = false;
void boardInit() {}
void boardBacklight(uint8_t) {}
void boardLvglInit() {}
void startNetwork() {}
bool readSnapshot(Snapshot &out, NetworkState &state)
{
    if (!publishedSnapshot) {
        return false;
    }
    out = *publishedSnapshot;
    state = networkState;
    return true;
}
bool requestNodeSelection(const char *)
{
    return false;
}
bool readConnectionSettings(ConnectionSettings &out)
{
    out = ConnectionSettings{};
    out.available = true;
    strcpy(out.ssid, "HomeWiFi");
    strcpy(out.endpoint, "http://collector.lan:8787/snapshot");
    out.displayTokenSaved = true;
    return true;
}
bool requestConnectionUpdate(const ConnectionUpdate &)
{
    return true;
}
bool readNodeConfiguration(NodeConfiguration &out)
{
    out = NodeConfiguration{};
    out.available = true;
    strcpy(out.version, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");  // pragma: allowlist secret (demo)
    strcpy(out.localId, "local");
    strcpy(out.localName, "Collector host");
    strcpy(out.localType, "proxmox");
    strcpy(out.localPlatform, "linux");
    out.localEnabled = true;
    const char *ids[] = {"local", "printer", "mac-studio"}, *names[] = {"Virtual host", "Workshop printer", "Mac Studio"};
    const char *types[] = {"local-proxmox", "klipper", "server-agent"}, *platforms[] = {"linux", "", "macos"};
    for (int i = 0; i < 3; ++i) {
        ConfigNode &n = out.nodes[out.count++];
        strcpy(n.id, ids[i]);
        strcpy(n.name, names[i]);
        strcpy(n.type, types[i]);
        strcpy(n.platform, platforms[i]);
        if (i == 1) {
            strcpy(n.url, "http://printer.lan:7125");
        }
        n.hasSecret = i == 1;
        n.enabled = i != 2;
    }
    return true;
}
bool requestNodeConfiguration()
{
    return true;
}
bool requestNodeUpsert(const NodeConfigDraft &, const char *)
{
    return true;
}
bool requestNodeDelete(const char *, const char *)
{
    return true;
}
const char *homelabPlatformSsid()
{
    return "HomeWiFi";
}
const char *homelabPlatformPassword()
{
    return "";
}

namespace {
constexpr int SIZE = 480;
std::vector<uint16_t> framebuffer(SIZE * SIZE);
alignas(4) uint8_t draw_buffer[SIZE * 40 * 2];
std::filesystem::path output;

void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = reinterpret_cast<uint16_t *>(pixels);
    const int stride = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            framebuffer[y * SIZE + x] = source[(y - area->y1) * stride + x - area->x1];
        }
    lv_display_flush_ready(display);
}

void write_ppm(const std::filesystem::path &path)
{
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << SIZE << " " << SIZE << "\n255\n";
    for (uint16_t v : framebuffer) {
        const uint8_t px[3] = {uint8_t(((v >> 11) & 31) * 255 / 31), uint8_t(((v >> 5) & 63) * 255 / 63),
                               uint8_t((v & 31) * 255 / 31)
                              };
        out.write(reinterpret_cast<const char *>(px), 3);
    }
}

void save(const std::string &name, lv_display_t *display)
{
    for (int i = 0; i < 80; ++i) {  // let entrances and springs settle
        nativeClock += 30;
        (lv_timer_handler)();
    }
    lv_obj_invalidate(lv_display_get_screen_active(display));
    lv_refr_now(display);
    write_ppm(output / (name + ".ppm"));
    std::cout << name << ".ppm\n";
}

// Animations, recorded only when asked (MOSAICO_PREVIEW_RECORD=1): frames
// every step_ms of UI time into output/NAME/0000.ppm on, appended across
// calls; each(i) runs before frame i.
bool recording = false;
void record(const std::string &name, int frames, int step_ms, const std::function<void(int)> &each = nullptr)
{
    if (!recording) {
        return;
    }
    static std::map<std::string, int> next;
    std::filesystem::create_directories(output / name);
    lv_display_t *display = lv_display_get_default();
    lv_obj_invalidate(lv_display_get_screen_active(display));
    for (int i = 0; i < frames; ++i) {
        if (each) {
            each(i);
        }
        nativeClock += uint32_t(step_ms);
        (lv_timer_handler)();
        lv_refr_now(display);
        char file[16];
        snprintf(file, sizeof(file), "%04d.ppm", next[name]++);
        write_ppm(output / name / file);
    }
    std::cout << name << ": " << next[name] << " frames\n";
}

// The scrollable object with the most to scroll on the active screen.
lv_obj_t *scroller(lv_obj_t *o, lv_obj_t *best = nullptr)
{
    const auto range = [](lv_obj_t *x) {
        return x ? lv_obj_get_scroll_top(x) + lv_obj_get_scroll_bottom(x) : -1;
    };
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && range(o) > range(best)) {
            best = o;
        }
        for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
            best = scroller(lv_obj_get_child(o, int32_t(i)), best);
        }
    }
    return best;
}

// Eases a scroll to y over the frames of a recording.
std::function<void(int)> scroll_to(int y, int frames)
{
    return [y, frames](int i) {
        lv_obj_t *s = scroller(lv_display_get_screen_active(lv_display_get_default()));
        if (!s || i >= frames) {
            return;
        }
        static int from = 0;
        if (i == 0) {
            from = lv_obj_get_scroll_y(s);
        }
        const float t = float(i + 1) / frames, k = t < 0.5f ? 2 * t * t : 1 - powf(-2 * t + 2, 2) / 2;
        lv_obj_scroll_to_y(s, int32_t(lroundf(from + (y - from) * k)), LV_ANIM_OFF);
    };
}

lv_obj_t *frame_screen(lv_obj_t **frame, int x, int y, int w, int h)
{
    lv_obj_t *screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *f = lv_obj_create(screen);
    lv_obj_remove_style_all(f);
    (lv_obj_set_pos)(f, x, y);
    (lv_obj_set_size)(f, w, h);
    lv_obj_remove_flag(f, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    *frame = f;
    return screen;
}

// A finger for the launcher's gestures: LVGL reads it like the touch panel.
struct {
    bool down = false;
    lv_point_t at{};
} finger;

void finger_read(lv_indev_t *, lv_indev_data_t *data)
{
    data->point = finger.at;
    data->state = finger.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Runs the UI for ms, 10 ms at a time.
void run_for(int ms)
{
    for (int t = 0; t < ms; t += 10) {
        nativeClock += 10;
        (lv_timer_handler)();
    }
}

// Down at a, along to b over ms (holding first for hold_ms), then up.
void gesture(lv_point_t a, lv_point_t b, int hold_ms, int ms, bool lift = true)
{
    finger.at = a;
    finger.down = true;
    run_for(hold_ms);
    const int steps = std::max(1, ms / 10);
    for (int i = 1; i <= steps; ++i) {
        finger.at = {a.x + (b.x - a.x) * i / steps, a.y + (b.y - a.y) * i / steps};
        run_for(10);
    }
    if (lift) {
        finger.down = false;
        run_for(400);
    }
}

void tap(lv_point_t at)
{
    gesture(at, at, 60, 0);
}

void print_layout(const char *when)
{
    char text[600];
    launcher_ui_layout(text, sizeof(text));
    std::cout << when << ": " << text << "\n";
}

void movie_scene(const char *key, void *)
{
    launcher_shortcut_feedback(key, 1);
}

// Swifty's still for the launcher, as the board renders it (swifty_app_portrait).
const lv_image_dsc_t *swifty_portrait(int size)
{
    static std::vector<uint8_t> px;
    static lv_image_dsc_t dsc{};
    px.assign(size_t(size) * size * 4, 0);
    swifty::Figure fig(size, 2);
    for (int i = 0; i < 90; ++i) {
        fig.step(1 / 30.f);
    }
    fig.render(px.data());
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc.header.w = uint32_t(size);
    dsc.header.h = uint32_t(size);
    dsc.header.stride = uint32_t(size * 4);
    dsc.data = px.data();
    dsc.data_size = uint32_t(px.size());
    return &dsc;
}

void render_launcher(int radius)
{
    lv_display_t *display = lv_display_get_default();
    LauncherHooks hooks{};
    hooks.wifi = wifi_settings_open;  // the Wi-Fi card's chevron
    lv_obj_t *launcher = launcher_ui_build(hooks, radius, 80, true, true, swifty_portrait(168));
    glimdock_summary_t glim;
    glimdock_app_summary(&glim);
    glim.started = true;
    static const float wave[] = {22, 24, 30, 27, 33, 38, 31, 26, 28, 35, 44, 39, 30, 25, 21, 24, 29, 36, 41, 34, 28, 22, 19, 23};
    memcpy(glim.history, wave, sizeof(wave));
    glim.history_count = 24;
    glim.ready = true;
    glim.alerts = 3;  // the icon badge
    home_summary_t home{};
    home.started = home.paired = home.connected = true;
    strcpy(home.title, "All is well at home.");
    home.lights = true;
    home.lights_pct = 68;
    home.lights_on = 2;
    home.lights_count = 3;
    home.lights_hue = 34;
    strcpy(home.lights_state, "2 of 3 on");
    home.music = true;
    home.volume = 20;
    strcpy(home.music_state, "Idle");
    home.tv = true;
    strcpy(home.tv_name, "Living room TV");
    strcpy(home.tv_state, "Off \xC2\xB7 HDMI 1");
    home.plug_count = 2;
    strcpy(home.plugs[0].name, "Desk lamp");
    home.plugs[0].on = home.plugs[0].known = true;
    strcpy(home.plugs[1].name, "Heater");
    home.plugs[1].known = true;
    LauncherStatus status{"20:41", "Sun 23 Nov", true, true, "HomeWiFi", "192.0.2.42", "Firmware 1.1.0", 84, true};
    status.camera_auto = true;  // the board's default
    status.always_on = 120;      // the always-on display's default: 2 minutes
    camera_summary_t camera{};
    swifty_summary_t swifty{};
    swifty.configured = true;
    strcpy(swifty.status, "Ready");
    strcpy(swifty.last, "**Good evening!** The living room lights are at 68% and the TV is off. Want me to dim them?");
    files_summary_t files{};
    files.storage = true;
    files.shares = 2;
    strcpy(files.status, "NAND: 92 of 118 MB free");
    camera.photos = 7;
    launcher_ui_update(status, glim, home, camera, swifty, files);
    lv_screen_load(launcher);
    launcher_ui_show_page(0);
    save("launcher-home-screen", display);
    // The VPN's badge after the Wi-Fi icon: connected, then connecting.
    for (const auto &[state, name] : {
                std::pair{LauncherVpn::Connected, "launcher-vpn"}, std::pair{LauncherVpn::Connecting, "launcher-vpn-connecting"}
            }) {
        status.vpn = state;
        launcher_ui_update(status, glim, home, camera, swifty, files);
        save(name, display);
    }
    status.vpn = LauncherVpn::Off;
    launcher_ui_update(status, glim, home, camera, swifty, files);
    print_layout("start");
    launcher_ui_show_page(6);
    save("launcher-settings", display);
    // Settings > Wi-Fi: the networks once the scan is done, then a password.
    wifi_settings_open();
    run_for(1600);
    save("launcher-wifi", display);
    tap({240, 430});  // Neighbour 5G (the fourth row): secured, not saved
    run_for(400);
    save("launcher-wifi-password", display);
    wifi_settings_close();
    launcher_ui_show_page(0);

    // The pages: a swipe to the second, with Webcam, Interaction and Calculator.
    gesture({400, 300}, {80, 300}, 20, 200);
    run_for(600);
    save("launcher-page-2", display);
    print_layout("swiped");
    // Hold Calculator: editing, and it follows the finger to the first slot.
    gesture({290, 100}, {90, 110}, 600, 300, false);
    save("launcher-drag", display);
    finger.down = false;
    run_for(400);
    save("launcher-edit", display);
    print_layout("calculator first");
    // Webcam (now second) to the left edge: the page turns, and it drops on
    // the first page's second row.
    gesture({190, 100}, {12, 330}, 60, 300, false);
    run_for(1200);
    finger.at = {150, 330};
    run_for(300);
    save("launcher-drag-across", display);
    finger.down = false;
    run_for(500);
    print_layout("webcam across");
    // − on Gallery hides it; + shows the hidden apps.
    tap({59, 303});
    run_for(300);
    save("launcher-edit-hidden", display);
    print_layout("gallery hidden");
    tap({74, 25});
    save("launcher-hidden-apps", display);
    tap({90, 148});
    print_layout("gallery back");
    tap({78, 36});  // back
    // Done.
    tap({384, 25});
    run_for(400);
    print_layout("done");
    save("launcher-after-edit", display);
    // A shortcut tile, the way the shortcut engine adds one, and its badge.
    static const mui::Accent warm = {0xFFD9A0, 0xB07A1E, 0xF3B56A, 0x2A1A05, 0x3C2C14, 0x140E06};
    launcher_shortcut_set({"sc:movie", "Movie", "tv", &warm, movie_scene, nullptr});
    launcher_ui_show_page(1);
    run_for(300);
    print_layout("shortcut");
    save("launcher-shortcut", display);
    launcher_ui_set_layout("");
    launcher_shortcut_remove("sc:movie");
    launcher_ui_show_page(0);
    print_layout("reset");
    if (recording) {
        launcher_ui_entered(ShellApp::Launcher);
        record("launcher", 60, 33);
        // A swipe to the second page and back.
        const auto swipe = [](int from, int step) {
            return [from, step](int i) {
                finger.down = i < 10;
                finger.at = {int32_t(from + step * std::min(i, 9)), 300};
            };
        };
        record("launcher", 30, 33, swipe(400, -32));
        record("launcher", 20, 33);
        record("launcher", 30, 33, swipe(80, 32));
        launcher_ui_show_page(6);
        record("launcher", 24, 33);
        // Settings > Wi-Fi: the scan, the networks, a password.
        wifi_settings_open();
        record("launcher", 54, 33);
        record("launcher", 4, 33, [](int i) {
            finger.down = i < 2;
            finger.at = {240, 430};  // Neighbour 5G
        });
        record("launcher", 36, 33);
        wifi_settings_close();
        record("launcher", 10, 33);
        record("launcher", 40, 33, scroll_to(520, 30));  // down to Web settings
        status.web_on = true, status.web_url = "http://192.0.2.42", status.web_code = "482913";
        launcher_ui_update(status, glim, home, camera, swifty, files);
        record("launcher", 50, 33);
        save("launcher-settings-web", display);
        status.web_on = false;
        launcher_ui_update(status, glim, home, camera, swifty, files);
        scroll_to(0, 1)(0);
        launcher_ui_show_page(0);
    }
    LauncherStatus offline{"--:--", "", false, false, "", "", "Firmware 1.1.0", -1, false};
    glimdock_summary_t none{};
    none.cpu = none.memory = NAN;
    strcpy(none.status, "Homelab monitor");
    home_summary_t no_home{};
    strcpy(no_home.title, "Swifty Home");
    swifty_summary_t no_swifty{};
    files_summary_t no_files{};
    launcher_ui_update(offline, none, no_home, camera, no_swifty, no_files);
    launcher_ui_show_page(0);
    save("launcher-empty", display);
    launcher_ui_show_page(6);
    save("launcher-settings-offline", display);
    launcher_ui_calibration(true, radius);
    save("launcher-corners", display);
    launcher_ui_calibration(false, radius);
}

// The shortcuts' screens (shortcuts_ui.cpp over host/shortcuts_host.cpp's
// demo shortcuts): the editor's list, a shortcut's page and its run's log,
// New, the card that asks before an automation, a message and a toast.
void render_shortcuts()
{
    lv_display_t *display = lv_display_get_default();
    lv_obj_t *screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);
    shortcuts::ui_editor_open();
    save("shortcuts-list", display);
    tap({240, 100 + 76 + 33});  // Evening, the second row
    run_for(300);
    save("shortcuts-detail", display);
    lv_obj_t *s = scroller(lv_layer_top());
    if (s) {
        lv_obj_scroll_to_y(s, 2000, LV_ANIM_OFF);
    }
    save("shortcuts-detail-log", display);
    shortcuts::ui_editor_back();
    tap({240, 100 + 2 * 76 + 33});  // Hall motion: an automation that is off
    run_for(300);
    save("shortcuts-automation", display);
    shortcuts::ui_editor_back();
    tap({240, 425});  // New shortcut
    run_for(300);
    save("shortcuts-new", display);
    shortcuts::ui_editor_back();
    shortcuts::ui_editor_back();
    shortcuts::ui_ask("Evening", "Swifty wants this to run by itself: 15 min before sunset; At 19:30 on weekdays.\n"
                      "If Desk lamp is off:\n   Living room lights on\nRepeat 3 times:", "Allow", 60, [](bool) {});
    save("shortcuts-ask", display);
    tap({128, 72 + 262 + 25});  // Not now
    run_for(300);
    shortcuts::ui_message("Hall motion", "Someone is in the hall: the lights stay on for two minutes.", 3, true, false);
    save("shortcuts-message", display);
    shortcuts::ui_toast("Movie", "tv", 1, shortcuts::Feedback::Failed, "Step 2: No Interaction board found");
    save("shortcuts-toast", display);
    run_for(13000);  // the message and the toast go
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: mosaico-preview OUTPUT_DIR [RADIUS] [HOME_FIXTURES]\n";
        return 2;
    }
    output = argv[1];
    std::filesystem::create_directories(output);
    const int radius = argc > 2 ? atoi(argv[2]) : 80;
    recording = getenv("MOSAICO_PREVIEW_RECORD") != nullptr;
    lv_init();
    lv_tick_set_cb(millis);
    lv_display_t *display = lv_display_create(SIZE, SIZE);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, draw_buffer, nullptr, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, finger_read);

    // Glimdock over the whole panel, from the firmware's demo data.
    static Snapshot data;
    loadFixture(data, networkState);
    publishedSnapshot = &data;
    render_launcher(radius);
    render_shortcuts();
    {
        tools_hooks_t th = {};
        th.gravity = [](float * x, float * y, float * z) {
            // Still for the captures; a slow tilt around while recording.
            const float a = recording ? float(nativeClock % 4000) / 4000.0f * 6.2832f : 0.0f;
            const float r = recording ? 0.22f : 0.067f;
            *x = recording ? r * cosf(a) : 0.06f, *y = recording ? r * sinf(a) : -0.03f;
            *z = sqrtf(1 - *x * *x - *y * *y);
            return true;
        };
        tools_app_init(&th);
        lv_obj_t *tools = lv_obj_create(nullptr);
        lv_obj_remove_style_all(tools);
        lv_obj_set_style_bg_color(tools, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(tools, LV_OPA_COVER, 0);
        lv_screen_load(tools);
        static const char *const tool_names[] = {"tools-torch", "tools-level", "tools-compass", "calculator", "interaction", "webcam"};
        for (int i = 0; i < 6; ++i) {
            tools_app_open_tool(tools, i);
            for (int k = 0; k < 20; ++k) {
                nativeClock += 60;
                (lv_timer_handler)();
            }
            save(tool_names[i], lv_display_get_default());
            if (i == 1) {
                record("level", 120, 33);
            }
        }
        tools_app_set_active(false);
    }
    {
        void vpn_host_demo(bool on);
        vpn_app_init(nullptr);
        lv_obj_t *vpn = lv_obj_create(nullptr);
        lv_obj_remove_style_all(vpn);
        lv_obj_set_style_bg_color(vpn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(vpn, LV_OPA_COVER, 0);
        lv_screen_load(vpn);
        for (int on = 0; on < 2; ++on) {
            vpn_host_demo(on);
            vpn_app_open(vpn);
            for (int k = 0; k < 20; ++k) {
                nativeClock += 60;
                (lv_timer_handler)();
            }
            save(on ? "vpn-connected" : "vpn-off", lv_display_get_default());
        }
        vpn_app_set_active(false);
        // Games: the picker, each title screen with its game playing itself,
        // a game under way, paused and over.
        games_hooks_t gh = {};
        games_app_init(&gh);
        lv_obj_t *games = lv_obj_create(nullptr);
        lv_obj_remove_style_all(games);
        lv_obj_set_style_bg_color(games, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(games, LV_OPA_COVER, 0);
        lv_screen_load(games);
        games_app_open(games);
        games_app_preview_best(0, 3480);
        games_app_preview_best(1, 12650);
        const auto frames = [](int n, const char *recording = nullptr) {
            for (int i = 0; i < n; ++i) {
                games_app_preview_frame();
                nativeClock += 33;
                (lv_timer_handler)();
                if (recording) {
                    record(recording, 1, 0);
                }
            }
        };
        games_app_preview_mode(0);
        frames(3);
        save("games-picker", lv_display_get_default());
        static const char *const names[6] = {"crypt", "starbore", "skyglide", "jellystorm", "pinball", "rally"};
        for (int g = 0; g < 6; ++g) {
            games_app_select(g);
            frames(g == 2 || g >= 3 ? 120 : 90);
            save(std::string("games-title-") + names[g], lv_display_get_default());
            games_app_preview_mode(2);
            frames(g == 0 ? 260 : g == 4 ? 300 : 200);
            save(std::string("games-play-") + names[g], lv_display_get_default());
            if (recording) {
                frames(150, (std::string("games-") + names[g]).c_str());
            }
            if (g == 1) {
                games_app_preview_mode(3);
                frames(2);
                save("games-paused", lv_display_get_default());
            }
            if (g == 2) {
                games_app_preview_mode(4);
                frames(3);
                save("games-over", lv_display_get_default());
            }
        }
        games_app_set_active(false);
    }
    lv_obj_t *glim = lv_obj_create(nullptr);
    lv_obj_remove_style_all(glim);
    lv_obj_set_style_bg_color(glim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(glim, LV_OPA_COVER, 0);
    lv_screen_load(glim);
    glim_ui_start(glim);
    // Pages: 0 systems, 1 clock, 2 settings, 3 overview, 4 guests, 5 storage,
    // 6 sensors, 7 health, 8 job, 9 temps. Demo nodes: 0 host, 1 printer, 2 Mac.
    const struct {
        int page, node;
        bool demo;
        const char *name;
    } shots[] = {
        {0, -1, false, "glim-setup"}, {0, -1, true, "glim-systems"}, {1, -1, true, "glim-clock"},
        {2, -1, true, "glim-settings"}, {3, 0, true, "glim-overview"}, {4, 0, true, "glim-guests"},
        {5, 0, true, "glim-storage"}, {6, 0, true, "glim-sensors"}, {7, 0, true, "glim-health"},
        {8, 1, true, "glim-printer-job"}, {9, 1, true, "glim-printer-temps"}, {3, 2, true, "glim-mac"}
    };
    for (const auto &shot : shots) {
        if (!shot.demo) {  // not set up: no collector, no data
            publishedSnapshot = nullptr;
            networkState = NetworkState{};
        }
        glim_ui_preview(shot.page, shot.node, shot.demo);
        save(shot.name, display);
        publishedSnapshot = &data;
    }
    glim_ui_preview(3, 0, true);
    glim_ui_preview_overlay(0);
    save("glim-picker", display);
    glim_ui_preview_overlay(1);
    save("glim-connection", display);
    glim_ui_preview_overlay(2);
    save("glim-keyboard", display);
    glim_ui_preview(10, -1, true);
    save("glim-agents", display);
    glim_ui_preview_overlay(5);
    save("glim-agents-cards", display);
    glim_ui_preview_overlay(6);
    save("glim-agents-chat", display);
    glim_ui_preview(10, -1, true);
    glim_ui_preview(11, -1, true);
    save("glim-nodes", display);
    glim_ui_preview_overlay(3);
    save("glim-node-form", display);
    glim_ui_preview(0, -1, true);
    glim_ui_preview_overlay(4);
    save("glim-popup", display);
    if (recording) {
        // 0 systems, 3 overview of the host, 9 the printer's temperatures, 1 clock, 10 agents.
        const int tour[][2] = {{0, -1}, {3, 0}, {9, 1}, {1, -1}, {10, -1}};
        for (const auto &stop : tour) {
            glim_ui_preview(stop[0], stop[1], true);
            record("glimdock", 50, 33);
        }
    }
    if (recording) {
        constexpr int BIRD = 340;
        static std::vector<uint8_t> px(size_t(BIRD) * BIRD * 4);
        static lv_image_dsc_t dsc{};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        dsc.header.w = dsc.header.h = BIRD;
        dsc.header.stride = BIRD * 4;
        dsc.data = px.data();
        dsc.data_size = uint32_t(px.size());
        lv_obj_t *screen = lv_obj_create(nullptr);
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_t *img = lv_image_create(screen);
        lv_obj_set_pos(img, (SIZE - BIRD) / 2, 40);
        lv_obj_t *caption = mui::label(screen, "", 40, 392, 400, mui::TEXT2, 20, true, LV_TEXT_ALIGN_CENTER);
        lv_screen_load(screen);
        swifty::Figure fig(BIRD, 2);
        const struct {
            int frames;
            const char *text;
            std::function<void(int)> start;
        } acts[] = {
            {
                70, "Hello!", [&](int)
                {
                    fig.shoot(swifty::Shot::Hello);
                }
            },
            {
                75, "Listening\xE2\x80\xA6", [&](int)
                {
                    fig.set_mood(swifty::Mood::Listen);
                }
            },
            {
                55, "Thinking\xE2\x80\xA6", [&](int)
                {
                    fig.set_mood(swifty::Mood::Think);
                }
            },
            {
                90, "Lights are at 68%. Dim them?", [&](int)
                {
                    fig.react(swifty::Shot::Pleased, false, 160);
                }
            },
            {
                50, "", [&](int)
                {
                    fig.set_mood(swifty::Mood::Idle);
                }
            },
        };
        for (const auto &act : acts) {
            act.start(0);
            lv_label_set_text(caption, act.text);
            record("swifty", act.frames, 33, [&](int i) {
                fig.set_level(0.35f + 0.3f * sinf(float(i) * 0.7f) * sinf(float(i) * 0.23f));
                fig.step(0.033f);
                fig.render(px.data());
                lv_image_set_src(img, nullptr);
                lv_image_set_src(img, &dsc);
            });
        }
    }
    // Swifty Home in the inscribed square.
    if (argc > 3) {
        home_fixture_dir = argv[3];
        const int home_x = int(lroundf(radius * 0.15f)), home_y = int(lroundf(radius * 0.1f));
        lv_obj_t *home_frame = nullptr;
        lv_obj_t *home_screen = frame_screen(&home_frame, home_x, home_y, SIZE - 2 * home_x, SIZE - 2 * home_y);
        lv_screen_load(home_screen);
        home_app_open(home_frame);
        for (int i = 0; i < 400 && !(home::hub.state && home::hub.devices && !home::hub.catalog.empty()); ++i) {
            nativeClock += 50;
            (lv_timer_handler)();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const std::pair<int, const char *> pages[] = {{2, "home"}, {3, "home-lights"}, {4, "home-music"},
            {5, "home-pets"}, {7, "home-settings"}, {0, "home-pair"}
        };
        home::home_preview_theme(1);  // AMOLED
        for (auto &page : pages) {
            home::home_preview_open(page.first);
            save(page.second, display);
        }
        for (int page : {
                    2, 3, 4
                }) {
            home::home_preview_open(page);
            record("home", 55, 33);
        }
        home::home_preview_theme(0);
        home::home_preview_open(2);
        save("light-home", display);
        home::home_preview_theme(1);
    }
    return 0;
}
