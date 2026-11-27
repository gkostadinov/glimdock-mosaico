// SPDX-License-Identifier: Apache-2.0
// Auto-rotate: the BMI270 says which panel edge is down, and the whole UI
// turns to keep its bottom there. The panel and touch turn through MADCTL
// (bsp_display_set_rotation), which is instant, so the turn is staged: the
// screen is snapshotted, shown on the top layer counter-rotated so nothing
// moves on the glass, and then turned to upright, as an iPhone does.
#include "orientation.h"
#include "frame_sync.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"

namespace {
const char *TAG = "orient";
constexpr const char *PREFS = "mshell";
constexpr uint16_t RPC_SERVICE = 0x4744, RPC_ORIENT = 10;
constexpr int POLL_MS = 50, SETTLE_MS = 380;
// Gravity must lie within this many degrees of an edge's axis to pick that
// edge (45 would flip back and forth at the diagonal).
constexpr float CAPTURE_DEG = 32.0f, MIN_TILT_G = 0.45f;

// IMU axes in panel terms at rotation 0 (x right, y down), packed: bits 0-1
// the accelerometer axis giving gravity along x, bit 2 negate it; bits 3-4
// and bit 5 the same for y. 8: x = +ax, y = +ay (from the owner's checks on
// the board: map 1 was a quarter turn off, map 40 mirrored one axis).
constexpr uint8_t DEFAULT_MAP = 8;

struct {
    volatile bool enabled = true, imu = false;
    volatile int forced = -1;  // rotation in degrees from the RPC, -1 automatic
    volatile int board = 0;    // the rotation gravity calls for, auto-rotate on or off
    volatile uint8_t map = DEFAULT_MAP;
    float a[3] = {0, 0, 1};    // filtered accelerometer, g
    // Games (orientation_game_mode): the gyroscope, its rest bias, and the
    // turn about the vertical integrated from it.
    volatile bool game = false;
    bool gyro = false;
    float w[3] = {}, bias[3] = {};
    float yaw = 0;             // rad, + clockwise seen from above
    float hand = 1, hand_score = 0;  // the gyro's sense against the accelerometer, learned from tilts
    float still_sum[3] = {};
    int still_n = 0;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    lv_obj_t *overlay = nullptr, *image = nullptr;
    lv_draw_buf_t *snapshot = nullptr;
    int from = 0;              // overlay start angle, 0.1 degree
    TaskHandle_t task = nullptr;
} O;

void load_prefs()
{
    static bool loaded = false;
    if (loaded) {
        return;
    }
    loaded = true;
    nvs_handle_t h;
    if (nvs_open(PREFS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v;
    if (nvs_get_u8(h, "autorot", &v) == ESP_OK) {
        O.enabled = v;
    }
    if (nvs_get_u8(h, "imumap", &v) == ESP_OK) {
        O.map = v;
    }
    nvs_close(h);
}

void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(PREFS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

// Gravity in panel coordinates (x right, y down) at rotation 0.
void gravity(float *gx, float *gy)
{
    const uint8_t m = O.map;
    *gx = O.a[m & 3] * ((m & 4) ? -1.0f : 1.0f);
    *gy = O.a[(m >> 3) & 3] * ((m & 32) ? -1.0f : 1.0f);
}

// The edge that is down, as the rotation that puts the UI's bottom there; -1
// while the board lies flat or sits near a diagonal. BSP rotation 90 maps the
// UI's down onto the panel's right edge (see compose_orientation).
int pick(int current)
{
    float gx, gy;
    gravity(&gx, &gy);
    if (hypotf(gx, gy) < MIN_TILT_G) {
        return -1;
    }
    float deg = atan2f(gx, gy) * 180.0f / float(M_PI);  // 0 bottom, 90 right
    if (deg < 0) {
        deg += 360.0f;
    }
    const int nearest = (int(lroundf(deg / 90.0f)) % 4) * 90;
    float off = fabsf(deg - float(nearest));
    if (off > 180.0f) {
        off = 360.0f - off;
    }
    if (nearest != current && off > CAPTURE_DEG) {
        return -1;
    }
    return nearest;
}

// ---- the turn ----------------------------------------------------------------------------
void finish_turn()
{
    if (O.overlay) {
        lv_obj_delete(O.overlay);
    }
    O.overlay = O.image = nullptr;
    if (O.snapshot) {
        lv_draw_buf_destroy(O.snapshot);
    }
    O.snapshot = nullptr;
}

void spin_cb(void *, int32_t p)  // p: 0..1024
{
    if (!O.image) {
        return;
    }
    const float t = p / 1024.0f;
    lv_image_set_rotation(O.image, int32_t(lroundf(O.from * (1.0f - t))));
}

void fade_cb(void *, int32_t v)
{
    if (O.overlay) {
        lv_obj_set_style_opa(O.overlay, lv_opa_t(v), 0);
    }
}

void spun_cb(lv_anim_t *)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &O);
    lv_anim_set_exec_cb(&a, fade_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 90);
    lv_anim_set_completed_cb(&a, [](lv_anim_t *) {
        finish_turn();
    });
    lv_anim_start(&a);
}

void turn_to(int next)
{
    const int previous = int(bsp_display_get_rotation());
    if (next == previous) {
        return;
    }
    // Content turns clockwise by the rotation step on the glass; start the
    // overlay that far the other way so the first new frame looks unchanged,
    // the short way round.
    int delta = (next - previous + 360) % 360;
    if (delta > 180) {
        delta -= 360;
    }
    // Freeze rendering while the overlay goes up, so no frame shows it turned
    // on the old orientation.
    if (esp_lv_adapter_pause(1000) != ESP_OK) {
        return;
    }
    if (bsp_display_lock(1000)) {
        lv_anim_delete(&O, nullptr);
        finish_turn();
        O.snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        if (O.snapshot) {
            O.overlay = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(O.overlay);
            lv_obj_set_size(O.overlay, LV_PCT(100), LV_PCT(100));
            lv_obj_set_style_bg_color(O.overlay, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(O.overlay, LV_OPA_COVER, 0);
            lv_obj_remove_flag(O.overlay, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            O.image = lv_image_create(O.overlay);
            lv_image_set_src(O.image, O.snapshot);
            lv_obj_center(O.image);
            // Around the middle: the object's size is not laid out yet here, so
            // it read 0 and the turn swung around a corner.
            lv_image_set_pivot(O.image, O.snapshot->header.w / 2, O.snapshot->header.h / 2);
            lv_image_set_antialias(O.image, false);  // a quick turn: speed over smooth edges
            // BSP rotations run the other way to pick()'s angles (the IMU map
            // compensates, see the camera's want_mat), so the snapshot starts
            // turned by -delta: then it follows the hand, as on a phone.
            O.from = -delta * 10;
            lv_image_set_rotation(O.image, O.from);
        }
        bsp_display_unlock();
    }
    frame_sync_wait_idle(200);  // not while a frame is still going out
    const esp_err_t err = bsp_display_set_rotation(bsp_display_rotation_t(next));
    esp_lv_adapter_resume();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rotation %d failed: %s", next, esp_err_to_name(err));
    }
    // The overlay is up and turned: it must be spun or removed, so keep trying
    // for the lock while LVGL is busy.
    bool locked = false;
    for (int i = 0; i < 40 && !(locked = bsp_display_lock(250)); ++i) {
    }
    if (!locked) {
        ESP_LOGE(TAG, "display lock unavailable; turn overlay left up");
        return;
    }
    if (err != ESP_OK) {
        finish_turn();
    } else if (O.image) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, &O);
        lv_anim_set_exec_cb(&a, spin_cb);
        lv_anim_set_values(&a, 0, 1024);
        lv_anim_set_duration(&a, abs(delta) > 90 ? 400 : 300);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, spun_cb);
        lv_anim_start(&a);
    }
    bsp_display_unlock();
    ESP_LOGI(TAG, "rotated %d -> %d", previous, next);
}

// Games: the gyroscope every 10 ms. The turn about the vertical is the rate
// along gravity; the rest bias is taken while the board is still, and the
// gyro's sense against the accelerometer (a mirrored axis map would turn the
// view the wrong way) is learned from tilts: a world-fixed vector seen from
// the board moves as -w x a.
void sample_gyro(const float before[3], float dt)
{
    float gx, gy, gz;
    if (bsp_imu_get_gyro(&gx, &gy, &gz) != ESP_OK) {
        return;
    }
    constexpr float RAD = float(M_PI) / 180.0f;
    const float raw[3] = {gx * RAD, gy * RAD, gz * RAD};
    float w[3];
    for (int i = 0; i < 3; ++i) {
        w[i] = raw[i] - O.bias[i];
    }
    const float spin = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    const float da[3] = {O.a[0] - before[0], O.a[1] - before[1], O.a[2] - before[2]};
    const float moved = sqrtf(da[0] * da[0] + da[1] * da[1] + da[2] * da[2]);
    if (spin < 0.05f && moved < 0.01f) {  // still: average the rest rate for the bias
        for (int i = 0; i < 3; ++i) {
            O.still_sum[i] += raw[i];
        }
        if (++O.still_n >= 50) {
            for (int i = 0; i < 3; ++i) {
                O.bias[i] += 0.5f * (O.still_sum[i] / float(O.still_n) - O.bias[i]);
                O.still_sum[i] = 0;
            }
            O.still_n = 0;
        }
    } else {
        O.still_n = 0;
        for (float &v : O.still_sum) {
            v = 0;
        }
    }
    const float *a = O.a;
    if (spin > 0.6f) {
        const float cx = w[1] * a[2] - w[2] * a[1], cy = w[2] * a[0] - w[0] * a[2], cz = w[0] * a[1] - w[1] * a[0];
        O.hand_score = std::clamp(O.hand_score - (da[0] * cx + da[1] * cy + da[2] * cz) / dt, -50.0f, 50.0f);
        if (fabsf(O.hand_score) > 5) {
            O.hand = O.hand_score > 0 ? 1.0f : -1.0f;
        }
    }
    // the accelerometer reads "up" at rest: the rate about it is anticlockwise from above
    const float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    float up_rate = len > 0.3f ? (w[0] * a[0] + w[1] * a[1] + w[2] * a[2]) / len : 0;
    if (fabsf(up_rate) < 0.03f) {
        up_rate = 0;
    }
    portENTER_CRITICAL(&O.mux);
    memcpy(O.w, w, sizeof(w));
    O.yaw -= O.hand * up_rate * dt;
    O.gyro = true;
    portEXIT_CRITICAL(&O.mux);
}

void task(void *)
{
    int candidate = -1;
    TickType_t since = 0;
    for (;;) {
        const bool game = O.game;
        vTaskDelay(pdMS_TO_TICKS(game ? 10 : POLL_MS));
        float x, y, z;
        if (O.imu && bsp_imu_get_accel(&x, &y, &z) == ESP_OK) {
            const float k = game ? 0.5f : 0.3f;
            const float before[3] = {O.a[0], O.a[1], O.a[2]};
            portENTER_CRITICAL(&O.mux);
            O.a[0] += k * (x - O.a[0]);
            O.a[1] += k * (y - O.a[1]);
            O.a[2] += k * (z - O.a[2]);
            portEXIT_CRITICAL(&O.mux);
            if (game) {
                sample_gyro(before, 0.01f);
            }
            // Kept while flat or near a diagonal (pick's -1).
            const int board = pick(O.board);
            if (board >= 0) {
                O.board = board;
            }
        }
        const int current = int(bsp_display_get_rotation());
        int want = O.forced;
        if (want < 0) {
            want = O.enabled && O.imu ? pick(current) : (O.enabled ? -1 : 0);
        }
        if (game && O.forced < 0) {
            want = -1;  // a game steers by tilt: the UI keeps its rotation
        }
        if (want < 0 || want == current) {
            candidate = -1;
            continue;
        }
        const TickType_t now = xTaskGetTickCount();
        if (want != candidate) {
            candidate = want;
            since = now;
            continue;
        }
        if (O.forced < 0 && now - since < pdMS_TO_TICKS(SETTLE_MS)) {
            continue;
        }
        candidate = -1;
        turn_to(want);
    }
}

// Payload: none reads; 1 r forces rotation r*90 (r 4 = automatic); 2 e turns
// auto-rotate on/off; 3 m sets the axis map. Reply: text with the raw
// accelerometer, gravity in panel terms and the state.
esp_err_t rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    if (request->payload_size >= 2) {
        const uint8_t cmd = request->payload[0], v = request->payload[1];
        if (cmd == 1) {
            O.forced = v < 4 ? v * 90 : -1;
        } else if (cmd == 2) {
            orientation_set_enabled(v != 0);
        } else if (cmd == 3) {
            O.map = v;
            save_u8("imumap", v);
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    float gx, gy;
    gravity(&gx, &gy);
    const int n = snprintf(reinterpret_cast<char *>(out), capacity,
                           "accel %+.2f %+.2f %+.2f | panel gx %+.2f gy %+.2f | rotation %d | auto %d imu %d map %u forced %d | "
                           "game %d gyro %+.2f %+.2f %+.2f rad/s yaw %+.1f deg hand %+.0f (%+.1f) bias %+.3f %+.3f %+.3f",
                           O.a[0], O.a[1], O.a[2], gx, gy, int(bsp_display_get_rotation()), int(O.enabled),
                           int(O.imu), unsigned(O.map), O.forced, int(O.game), O.w[0], O.w[1], O.w[2],
                           O.yaw * 180 / float(M_PI), O.hand, O.hand_score, O.bias[0], O.bias[1], O.bias[2]);
    *size = n < 0 ? 0 : std::min<size_t>(size_t(n), capacity);
    return ESP_OK;
}
}  // namespace

void orientation_start()
{
    if (O.task) {
        return;
    }
    load_prefs();
    if (bsp_imu_init() == ESP_OK) {
        bsp_imu_config_t config = BSP_IMU_CONFIG_DEFAULT();
        O.imu = bsp_imu_start(&config) == ESP_OK;
    }
    if (!O.imu) {
        ESP_LOGW(TAG, "no IMU; auto-rotate off");
    }
    esp_iris_rpc_register(RPC_SERVICE, RPC_ORIENT, rpc, nullptr);
    // The turn renders a snapshot of the screen on this task (TrueType glyphs
    // included), which needs as much stack as the LVGL task; it never writes
    // flash, so the stack can live in PSRAM.
    // Above the LVGL draw thread, so it gets the display lock while a busy
    // screen (Swifty, the camera) keeps LVGL drawing.
    xTaskCreatePinnedToCoreWithCaps(task, "orient", 32 * 1024, nullptr, 5, &O.task, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool orientation_enabled()
{
    load_prefs();
    return O.enabled;
}

void orientation_to_ui(float *x, float *y, float *z)
{
    const float v[3] = {*x, *y, *z};
    const uint8_t m = O.map;
    const int ix = m & 3, iy = (m >> 3) & 3, iz = 3 - std::min(ix, 2) - std::min(iy, 2);
    const float px = v[std::min(ix, 2)] * ((m & 4) ? -1.0f : 1.0f), py = v[std::min(iy, 2)] * ((m & 32) ? -1.0f : 1.0f);
    // pick(): rotation r puts the UI's down where gravity points at angle r
    // (0 bottom, 90 right), so turn by -r in that sense.
    const float r = float(int(bsp_display_get_rotation())) * float(M_PI) / 180.0f, c = cosf(r), s = sinf(r);
    *x = px * c - py * s;
    *y = px * s + py * c;
    *z = v[std::clamp(iz, 0, 2)];
}

int orientation_board_rotation()
{
    return O.imu ? O.board : int(bsp_display_get_rotation());
}

bool orientation_gravity(float *x, float *y, float *z)
{
    if (!O.imu) {
        return false;
    }
    portENTER_CRITICAL(&O.mux);
    *x = O.a[0], *y = O.a[1], *z = O.a[2];
    portEXIT_CRITICAL(&O.mux);
    orientation_to_ui(x, y, z);
    return true;
}

void orientation_game_mode(bool on)
{
    if (on && !O.game) {
        O.hand_score = 0;  // learn afresh: cheap, and a wrong guess can't stick
    }
    O.game = on;
}

bool orientation_motion(float gravity[3], float rate[3], float *yaw, bool *gyro)
{
    if (!O.imu) {
        return false;
    }
    float a[3], w[3];
    portENTER_CRITICAL(&O.mux);
    memcpy(a, O.a, sizeof(a));
    memcpy(w, O.w, sizeof(w));
    *yaw = O.yaw;
    *gyro = O.gyro && O.game;
    portEXIT_CRITICAL(&O.mux);
    orientation_to_ui(&a[0], &a[1], &a[2]);
    orientation_to_ui(&w[0], &w[1], &w[2]);
    memcpy(gravity, a, sizeof(a));
    memcpy(rate, w, sizeof(w));
    return true;
}

void orientation_set_enabled(bool enabled)
{
    O.enabled = enabled;
    O.forced = -1;
    save_u8("autorot", enabled);
}
