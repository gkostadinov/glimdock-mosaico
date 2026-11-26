// SPDX-License-Identifier: Apache-2.0
// Host preview: 480x480 RGB565 captures of the launcher, Glimdock (the Mosaico
// UI over the firmware's demo data) and Swifty Home. Writes PPM files; render.py
// converts them and overlays the panel's rounded corners.
#include <chrono>
#include <ctime>
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
#include "tools_app.h"
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

void save(const std::string &name, lv_display_t *display)
{
    for (int i = 0; i < 80; ++i) {  // let entrances and springs settle
        nativeClock += 30;
        (lv_timer_handler)();
    }
    lv_obj_invalidate(lv_display_get_screen_active(display));
    lv_refr_now(display);
    std::ofstream out(output / (name + ".ppm"), std::ios::binary);
    out << "P6\n" << SIZE << " " << SIZE << "\n255\n";
    for (uint16_t v : framebuffer) {
        const uint8_t px[3] = {uint8_t(((v >> 11) & 31) * 255 / 31), uint8_t(((v >> 5) & 63) * 255 / 63),
                               uint8_t((v & 31) * 255 / 31)
                              };
        out.write(reinterpret_cast<const char *>(px), 3);
    }
    std::cout << name << ".ppm\n";
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
    lv_obj_t *launcher = launcher_ui_build({nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}, radius, 80, true, true,
                                           swifty_portrait(168));
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
    LauncherStatus status{"20:41", "Sun 23 Nov", true, true, "HomeWiFi", "192.0.2.42", "Firmware 1.0.0", 84, true};
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
    launcher_ui_show_page(6);
    save("launcher-settings", display);
    launcher_ui_show_page(0);
    LauncherStatus offline{"--:--", "", false, false, "", "", "Firmware 1.0.0", -1, false};
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
    lv_init();
    lv_tick_set_cb(millis);
    lv_display_t *display = lv_display_create(SIZE, SIZE);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, draw_buffer, nullptr, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Glimdock over the whole panel, from the firmware's demo data.
    static Snapshot data;
    loadFixture(data, networkState);
    publishedSnapshot = &data;
    render_launcher(radius);
    {
        tools_hooks_t th = {};
        th.gravity = [](float * x, float * y, float * z) {
            *x = 0.06f, *y = -0.03f, *z = 0.99f;
            return true;
        };
        tools_app_init(&th);
        lv_obj_t *tools = lv_obj_create(nullptr);
        lv_obj_remove_style_all(tools);
        lv_obj_set_style_bg_color(tools, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(tools, LV_OPA_COVER, 0);
        lv_screen_load(tools);
        static const char *const tool_names[] = {"tools-torch", "tools-level", "tools-compass", "tools-calc"};
        for (int i = 0; i < 4; ++i) {
            tools_app_select(i);
            tools_app_open(tools);
            for (int k = 0; k < 20; ++k) {
                nativeClock += 60;
                (lv_timer_handler)();
            }
            save(tool_names[i], lv_display_get_default());
        }
        tools_app_set_active(false);
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
        home::home_preview_theme(0);
        home::home_preview_open(2);
        save("light-home", display);
        home::home_preview_theme(1);
    }
    return 0;
}
