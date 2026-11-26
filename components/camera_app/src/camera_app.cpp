// SPDX-License-Identifier: Apache-2.0
// Camera: a capture task converts camera frames (UYVY) with the PPA into one
// of two 480x480 RGB565 buffers, applies the chosen filter, and the viewfinder
// image swaps to the finished one under the display lock. Photos are the
// filtered viewfinder frame as JPEG, kept in PSRAM. Photos taken with the
// Camera's own shutter are then synced to the NAS (the Files app's photos
// share) whenever it answers; photos taken for Swifty stay temporary. The
// pipeline (640x640
// centre crop, 270 degree rotation, vertical mirror) follows the BSP's
// camera_photo_app example for the left slot; the image is then turned with
// the UI's rotation (auto-rotate) so the world stays upright and photos come
// out upright as held. The module swivels to face away; which way it faces is
// worked out from how the picture turns when the board does (see facing).
#include "camera_app.h"
#include "driver/jpeg_encode.h"

#include <dirent.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "bsp/esp_mosaico.h"
#include "bsp/imu.h"
#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "files_app.h"
#include "mosaico_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mosaico_module_camera.h"
#include "mui.h"
#include "nvs.h"

namespace {
using namespace mui;
const char *TAG = "camera";
constexpr int W = 480, H = 480, CROP = 640, ALIGN = 128, QUALITY = 85, THUMB = 60;
constexpr size_t FRAME_BYTES = W * H * 2, MAX_JPEG = 256 * 1024, MAX_PHOTOS = 48, MAX_PHOTO_BYTES = 12 * 1024 * 1024;
constexpr uint16_t RPC_SERVICE = 0x4744, RPC_LIST = 20, RPC_READ = 21, RPC_FLASH = 22, RPC_SHOOT = 23, RPC_STATE = 24;
// The flash is lit at least this long, and for at least this many frames,
// before the photo frame: the driver keeps up to 8 frames queued (320 ms at
// 25 fps), which were all exposed before the light came on, and auto exposure
// then needs a few frames to settle on the lit scene.
constexpr uint32_t FLASH_LEAD_MS = 520, FLASH_LEAD_FRAMES = 12;
const Accent &ACC = AMBER;

// ---- filters ---------------------------------------------------------------------------------
enum Filter : uint8_t { NORMAL, VIVID, WARM, COOL, FADE, MONO, NOIR, SEPIA, FILTERS };
const char *const FILTER_NAMES[FILTERS] = {"Original", "Vivid", "Warm", "Cool", "Fade", "Mono", "Noir", "Sepia"};
// Per-channel curves for the colour filters, a luminance map for the rest.
struct Lut {
    uint8_t r[32], g[64], b[32];
    uint16_t y[256];
    bool luma;
};
Lut luts[FILTERS];

uint16_t pack(float r, float g, float b)
{
    auto c = [](float v, int max) {
        return uint16_t(lroundf(fminf(fmaxf(v, 0.0f), 1.0f) * max));
    };
    return uint16_t(c(r, 31) << 11 | c(g, 63) << 5 | c(b, 31));
}

void build_luts()
{
    auto curve = [](float v, Filter f, int channel) {  // v in 0..1
        switch (f) {
        case VIVID: return 0.5f + (v - 0.5f) * 1.28f;
        case WARM: return channel == 0 ? v * 1.08f + 0.03f : channel == 2 ? v * 0.86f : v * 1.02f;
        case COOL: return channel == 2 ? v * 1.10f + 0.04f : channel == 0 ? v * 0.88f : v;
        case FADE: return 0.12f + v * 0.78f;
        default: return v;
        }
    };
    for (int f = 0; f < FILTERS; ++f) {
        Lut &l = luts[f];
        l.luma = f >= MONO;
        for (int i = 0; i < 32; ++i) {
            l.r[i] = uint8_t(lroundf(fminf(fmaxf(curve(i / 31.0f, Filter(f), 0), 0.0f), 1.0f) * 31));
            l.b[i] = uint8_t(lroundf(fminf(fmaxf(curve(i / 31.0f, Filter(f), 2), 0.0f), 1.0f) * 31));
        }
        for (int i = 0; i < 64; ++i) {
            l.g[i] = uint8_t(lroundf(fminf(fmaxf(curve(i / 63.0f, Filter(f), 1), 0.0f), 1.0f) * 63));
        }
        for (int i = 0; i < 256; ++i) {
            float y = i / 255.0f;
            if (f == NOIR) {
                y = fminf(fmaxf(0.5f + (y - 0.5f) * 1.7f, 0.0f), 1.0f);
            }
            l.y[i] = f == SEPIA ? pack(y * 1.07f + 0.04f, y * 0.92f + 0.02f, y * 0.70f) : pack(y, y, y);
        }
    }
}

void apply_filter(uint16_t *px, Filter f)
{
    if (f == NORMAL) {
        return;
    }
    const Lut &l = luts[f];
    uint16_t *const end = px + W * H;
    if (l.luma) {
        for (; px < end; ++px) {
            const uint16_t v = *px;
            const uint32_t y = ((v >> 11) * 8 * 77 + ((v >> 5) & 63) * 4 * 150 + (v & 31) * 8 * 29) >> 8;
            *px = l.y[y > 255 ? 255 : y];
        }
    } else {
        for (; px < end; ++px) {
            const uint16_t v = *px;
            *px = uint16_t(l.r[v >> 11] << 11 | l.g[(v >> 5) & 63] << 5 | l.b[v & 31]);
        }
    }
}

// ---- orientation ---------------------------------------------------------------------------
// 2x2 integer maps on centred pixel coordinates (x right, y down):
// (x, y) -> (a x + b y, c x + d y).
struct Mat {
    int a, b, c, d;
    bool operator==(const Mat &o) const
    {
        return a == o.a && b == o.b && c == o.c && d == o.d;
    }
};
Mat mul(const Mat &p, const Mat &q)
{
    return {p.a * q.a + p.b * q.c, p.a * q.b + p.b * q.d, p.c * q.a + p.d * q.c, p.c * q.b + p.d * q.d};
}
constexpr Mat IDENTITY{1, 0, 0, 1}, MIRROR_X{-1, 0, 0, 1}, MIRROR_Y{1, 0, 0, -1}, TURN_180{-1, 0, 0, -1};
// How the UI's coordinates land on the glass for each BSP rotation (the
// BSP's compose_orientation): 90 sends the UI's x axis up and y right.
Mat ui_mat(int rotation)
{
    switch (rotation) {
    case 90: return {0, 1, -1, 0};
    case 180: return TURN_180;
    case 270: return {0, -1, 1, 0};
    default: return IDENTITY;
    }
}
// PPA settings, index bits 0-1 angle, bit 2 mirror x, bit 3 mirror y.
constexpr int PPA_ANGLE = 3, PPA_MX = 4, PPA_MY = 8;
Mat ppa_mats[16];
bool ppa_probed = false;

// ---- state -----------------------------------------------------------------------------------
struct Photo {
    uint8_t *jpeg;     // in PSRAM, or null when it lives in a file
    size_t size;
    uint32_t at;       // Unix time, 0 when unknown
    std::string path;  // on the NAND, empty for a RAM-only photo
    std::string nas;   // its name on the NAS once uploaded
    bool keep;         // taken with the Camera's shutter: synced to the NAS
};
constexpr int KEEP_IN_RAM = 6;  // uploaded photos whose JPEG stays in memory too
std::atomic<uint32_t> photos_revision{0};  // changes when photos do (the Gallery redraws)
TaskHandle_t uploader = nullptr;
std::atomic<bool> relist_wanted{true};  // list the NAS folder on the next pass
std::atomic<int> nas_list_state{0};     // 0 listing, 1 listed, -1 unreachable, -2 no share
constexpr const char *PHOTO_DIR = MOSAICO_STORAGE_DIR "/photos";
constexpr size_t KEEP_FREE = 4 * 1024 * 1024;  // never fill the NAND Vibe Mode shares

struct {
    bool started = false;
    volatile bool active = false, present = false, streaming = false, capture = false, mirror = false,
                  shutdown = false, captured = false;
    volatile bool back = false;  // the module swivelled to face away from the person
    volatile bool mirror_back = false;  // the flip button's state for the rear camera (mirror: the front's)
    char facing_why[72] = "saved";  // what last set the facing, and when (RPC 24)
    volatile bool auto_facing = true;  // the swivel and turn checks may change it (Settings)
    // USB webcam: frames go to sink while it is set (capture task only).
    void (*volatile webcam_sink)(const unsigned char *, size_t) = nullptr;
    volatile camera_webcam_view_t webcam_view = nullptr;
    jpeg_encoder_handle_t wc_encoder = nullptr;
    uint8_t *wc_jpeg = nullptr;
    size_t wc_jpeg_size = 0;
    // RGB565 between the PPA and the encoder; true: YUV420 (less to write and
    // read, no conversion in the encoder, but measured no faster: for tests).
    volatile bool wc_yuv = false;
    // The facing was seen (motion or a turn) since the camera opened: until
    // then the LED never fires, the screen flashes instead.
    volatile bool facing_known = false;
    volatile bool probed = false;  // a camera was looked for (before that: no "missing" message)
    volatile bool torch_only = false;  // the Tools app's torch: camera open, flash lit, no stream
    volatile bool capture_keep = false;  // the pending capture is the Camera's own (synced), not Swifty's
    volatile uint32_t stored_count = 0;  // photos stored so far (take_photo waits for the next)
    volatile uint8_t filter = NORMAL;
    // Flash: 0 off, 1 fires for the photo, 2 torch (stays lit while live).
    volatile uint8_t flash_mode = 0;
    bool torch_lit = false;
    // The sensor upright on the glass at UI rotation 0, as a PPA map.
    // Default: SC101IOT (1280x720), a quarter turn from what the BSP example
    // used for the OV3640.
    Mat upright = {0, 1, -1, 0};
    uint8_t fallback = 1 | PPA_MY;  // until the PPA is probed
    char status[48] = "Tap to open";
    mosaico_camera_handle_t camera = nullptr;
    ppa_client_handle_t ppa = nullptr;
    uint16_t *frames[2] = {}, *view = nullptr, *thumb_px = nullptr;
    int shown = 0;
    lv_image_dsc_t dsc[2] = {}, view_dsc = {}, thumb_dsc = {};
    uint8_t *jpeg = nullptr;
    std::mutex lock;  // photos
    std::vector<Photo> photos;
    size_t photo_bytes = 0;  // RAM-only photos
    unsigned next_number = 1;
    bool stored = false;      // photos go to the NAND
    int gallery = -1;  // photo index shown, -1 viewfinder
    lv_obj_t *screen = nullptr, *viewfinder = nullptr, *status_dot = nullptr, *status_label = nullptr,
              *thumb = nullptr, *thumb_image = nullptr, *flash_btn = nullptr, *mirror_btn = nullptr,
               *filter_btn = nullptr, *facing_btn = nullptr, *strip = nullptr, *flash_overlay = nullptr, *message = nullptr,
                *gallery_layer = nullptr, *gallery_title = nullptr, *filter_toast = nullptr, *photo_image = nullptr;
    lv_obj_t *chips[FILTERS] = {};
    volatile bool ui_dirty = false;
    volatile int last_luma = -1;
    volatile bool headless = false;  // streaming for camera_app_take_photo, screen hidden
    void (*pick)(int index) = nullptr;  // camera_app_pick
} G;

void set_status(const char *s)
{
    strlcpy(G.status, s, sizeof(G.status));
    G.ui_dirty = true;
}

// ---- pipeline (capture task) -----------------------------------------------------------
// Which way the PPA rotates and whether it mirrors before or after turning
// is not documented, so each of its 16 settings is measured once on a small
// image with a red mark right of centre and a green one below it.
bool probe_ppa()
{
    constexpr int N = 32;
    constexpr size_t BYTES = N * N * 2;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    auto *in = static_cast<uint16_t *>(heap_caps_aligned_calloc(ALIGN, 1, BYTES, caps));
    auto *out = static_cast<uint16_t *>(heap_caps_aligned_calloc(ALIGN, 1, BYTES, caps));
    bool ok = in && out;
    if (ok) {
        in[16 * N + 27] = 0xF800;
        in[27 * N + 16] = 0x07E0;
        esp_cache_msync(in, BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    auto unit = [](int v) {
        const float f = v - (N - 1) / 2.0f;
        return fabsf(f) < 4 ? 0 : f > 0 ? 1 : -1;
    };
    for (int i = 0; ok && i < 16; ++i) {
        memset(out, 0, BYTES);
        esp_cache_msync(out, BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        ppa_srm_oper_config_t op = {};
        op.in.buffer = in;
        op.in.pic_w = op.in.block_w = N;
        op.in.pic_h = op.in.block_h = N;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = out;
        op.out.buffer_size = BYTES;
        op.out.pic_w = N;
        op.out.pic_h = N;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = ppa_srm_rotation_angle_t(i & PPA_ANGLE);
        op.scale_x = op.scale_y = 1.0f;
        op.mirror_x = i & PPA_MX;
        op.mirror_y = i & PPA_MY;
        op.mode = PPA_TRANS_MODE_BLOCKING;
        ok = ppa_do_scale_rotate_mirror(G.ppa, &op) == ESP_OK;
        esp_cache_msync(out, BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        int rx = -1, ry = -1, gx = -1, gy = -1;
        for (int p = 0; ok && p < N * N; ++p) {
            const int r = out[p] >> 11, g = (out[p] >> 5) & 63;
            if (r > 20 && g < 24) {
                rx = p % N, ry = p / N;
            } else if (g > 40 && r < 10) {
                gx = p % N, gy = p / N;
            }
        }
        ok = ok && rx >= 0 && gx >= 0;
        if (ok) ppa_mats[i] = {unit(rx), unit(gx), unit(ry), unit(gy)};
    }
    heap_caps_free(in);
    heap_caps_free(out);
    if (ok) {
        for (int i = 0; i < 16; ++i)
            ESP_LOGI(TAG, "PPA angle %d mx %d my %d: [%d %d; %d %d]", (i & 3) * 90, (i & PPA_MX) ? 1 : 0,
                     (i & PPA_MY) ? 1 : 0, ppa_mats[i].a, ppa_mats[i].b, ppa_mats[i].c, ppa_mats[i].d);
    } else {
        ESP_LOGW(TAG, "PPA probe failed; the viewfinder will not follow auto-rotate");
    }
    return ok;
}

Mat transpose(const Mat &m)
{
    return {m.a, m.c, m.b, m.d};    // a rotation's inverse
}

// Sensor to UI coordinates for a UI rotation, from the owner's video (the
// phone, the speaker's logo and the cables behind it as references):
// - G.upright flipped vertically is the true view at UI rotation 0: the front
//   camera as others see you, the rear camera as a window. The module sits on
//   the board's left edge and turns about that (vertical) edge, so both sides
//   share it; nothing turns upside down. (G.upright itself left the front
//   upside down and, turned 180 degrees for the rear, the rear mirrored.)
// - The front shows a mirror by default (a selfie); the flip button gives its
//   true view. The rear shows the window by default; the flip mirrors it.
// - A mirror and a window keep the world still when the board turns; the
//   front's true view (and the mirrored rear) turn with it. ui_mat runs the
//   other way round to the board's turn (orientation.cpp's map compensates),
//   so a still world is ui * glass.
Mat want_mat(int rotation, bool back, bool flip)
{
    Mat glass = mul(MIRROR_Y, G.upright);
    if (back ? flip : !flip) {
        glass = mul(MIRROR_X, glass);
    }
    const Mat ui = ui_mat(rotation);
    return mul(flip ? transpose(ui) : ui, glass);
}

// The flip button: a horizontal flip of whatever the camera shows, remembered
// for each facing (want_mat's mirror is exactly that in the UI's coordinates).
bool mirrored()
{
    return G.back ? G.mirror_back : G.mirror;
}

// The PPA setting that shows the sensor upright in the UI's current rotation.
uint8_t ppa_setting()
{
    static int last_rotation = -1;
    static bool last_mirror = false, last_back = false;
    static uint8_t last = 0;
    const int rotation = int(bsp_display_get_rotation());
    const bool mirror = mirrored(), back = G.back;
    if (rotation == last_rotation && mirror == last_mirror && back == last_back) {
        return last;
    }
    last_rotation = rotation;
    last_mirror = mirror;
    last_back = back;
    last = uint8_t(G.fallback ^ (mirror ? PPA_MX : 0));
    if (!ppa_probed) {
        return last;
    }
    const Mat want = want_mat(rotation, back, mirror);
    for (uint8_t i = 0; i < 16; ++i)
        if (ppa_mats[i] == want) {
            return last = i;
        }
    ESP_LOGW(TAG, "no PPA setting for rotation %d", rotation);
    return last;
}

// ---- which way the camera faces ------------------------------------------------------------
// There is no switch on the swivel, but the picture tells: when the board turns
// a quarter (and the UI with it), a camera facing the person sees a still scene
// turn one way on the sensor, and one facing away sees it turn the other way.
// Small luma thumbnails of the sensor picture from before and after each turn
// are matched against both; a clear winner sets the facing.
constexpr int TN = 32, RING = 24;
constexpr int64_t BEFORE_FROM_US = 2600000, BEFORE_TO_US = 800000, AFTER_US = 450000;
struct Thumb {
    int64_t at = 0;
    float angle = 0;  // board_angle when taken
    uint8_t px[TN * TN];
};
float board_angle = 0;  // degrees the board has turned so far (any axis), from the gyro

void note_facing(const char *how)
{
    snprintf(G.facing_why, sizeof(G.facing_why), "%s at %llds: %s", how, (long long)(esp_timer_get_time() / 1000000),
             G.back ? "rear" : "front");
}
struct {
    Thumb ring[RING];
    int head = 0, count = 0, every = 0;
    int seen_rotation = -1, from = 0, to = 0;
    int64_t turned_at = 0;  // a UI turn to check, 0 none
} F;

void save_prefs();
void show_toast(const char *text);
void trust_facing(bool back);
volatile bool facing_toast = false;

// The centre square of the sensor frame (the part the viewfinder shows) as
// TN x TN luma, each pixel the mean of four samples.
void take_thumb(const mosaico_camera_frame_t &frame, Thumb &t)
{
    const uint32_t bpl = frame.bytes_per_line ? frame.bytes_per_line : frame.width * 2;
    const uint8_t *src = static_cast<const uint8_t *>(frame.data);
    const int ox = int((frame.width - CROP) / 2) & ~1, oy = int((frame.height - CROP) / 2) & ~1, step = CROP / TN;
    for (int y = 0; y < TN; ++y)
        for (int x = 0; x < TN; ++x) {
            int sum = 0;
            for (int k = 0; k < 4; ++k) {
                const int px = ox + x * step + (k & 1) * (step / 2) + step / 4, py = oy + y * step + (k >> 1) * (step / 2) + step / 4;
                sum += src[size_t(py) * bpl + size_t(px) * 2 + 1];  // UYVY: Y is every odd byte
            }
            t.px[y * TN + x] = uint8_t(sum / 4);
        }
}

// Normalised correlation of b with a seen through m (sensor coordinates,
// centred): b(q) against a(m^-1 q).
float match(const Thumb &a, const Thumb &b, const Mat &m)
{
    const Mat inv = transpose(m);
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    int n = 0;
    for (int y = 2; y < TN - 2; ++y)
        for (int x = 2; x < TN - 2; ++x) {
            const int qx = 2 * x - (TN - 1), qy = 2 * y - (TN - 1);  // doubled centred coordinates
            const int px = (inv.a * qx + inv.b * qy + (TN - 1)) / 2, py = (inv.c * qx + inv.d * qy + (TN - 1)) / 2;
            const double va = a.px[py * TN + px], vb = b.px[y * TN + x];
            sa += va, sb += vb, saa += va * va, sbb += vb * vb, sab += va * vb, ++n;
        }
    const double cov = sab - sa * sb / n, var = (saa - sa * sa / n) * (sbb - sb * sb / n);
    return var > 1.0 ? float(cov / sqrt(var)) : 0.0f;
}

void judge_turn()
{
    const Thumb &after = F.ring[(F.head + RING - 1) % RING];
    // Sensor picture after = T(before) when the facing assumed is right.
    const Mat t = mul(transpose(want_mat(F.to, G.back, mirrored())), want_mat(F.from, G.back, mirrored()));
    float same = -1, other = -1, still = -1;
    for (int i = 0; i < F.count; ++i) {
        const Thumb &before = F.ring[(F.head + RING - 1 - i) % RING];
        const int64_t age = F.turned_at - before.at;
        if (age < BEFORE_TO_US || age > BEFORE_FROM_US) {
            continue;
        }
        same = fmaxf(same, match(before, after, t));
        other = fmaxf(other, match(before, after, transpose(t)));
        still = fmaxf(still, match(before, after, IDENTITY));
    }
    ESP_LOGI(TAG, "turn %d -> %d: facing %s; match as assumed %.2f, other way %.2f, unturned %.2f", F.from, F.to,
             G.back ? "back" : "front", double(same), double(other), double(still));
    if (same > 0.45f && same > other + 0.2f && same > still + 0.1f) {
        G.facing_known = true;
        trust_facing(G.back);
        note_facing("turn confirmed");
    }
    if (other > 0.45f && other > same + 0.2f && other > still + 0.1f) {
        G.back = !G.back;
        G.facing_known = true;
        trust_facing(G.back);
        note_facing("turn");
        ESP_LOGI(TAG, "the camera faces %s", G.back ? "away" : "the person");
        save_prefs();
        facing_toast = true;
        G.ui_dirty = true;
    }
}

// Every third frame: keep a thumbnail; after a UI turn, judge it.
void track_facing(const mosaico_camera_frame_t &frame)
{
    const int rotation = int(bsp_display_get_rotation());
    const int64_t now = esp_timer_get_time();
    if (F.seen_rotation < 0) {
        F.seen_rotation = rotation;
    }
    if (rotation != F.seen_rotation) {
        F.from = F.seen_rotation, F.to = rotation, F.turned_at = now;
        F.seen_rotation = rotation;
    }
    if (++F.every % 3 && !(F.turned_at && now - F.turned_at >= AFTER_US)) {
        return;
    }
    take_thumb(frame, F.ring[F.head]);
    F.ring[F.head].at = now;
    F.ring[F.head].angle = board_angle;
    F.head = (F.head + 1) % RING;
    F.count = std::min(F.count + 1, RING);
    if (F.turned_at && now - F.turned_at >= AFTER_US) {
        if (ppa_probed && G.auto_facing && ((F.to - F.from + 360) % 180)) {
            judge_turn();    // quarter turns only
        }
        F.turned_at = 0;
    }
}

// ---- which way the camera faces, from motion -----------------------------------------------
// Held in the hand the board always moves a little. Turning about its upright
// axis (gyro y: up on the glass at rotation 0) shifts the picture sideways:
// shown upright as the front camera (unmirrored), the picture moves the same
// way as the turn when the camera faces the person, and the other way when it
// faces away (the back picture is the front one turned 180 degrees). The shift
// comes from column sums of the sensor picture, matched frame to frame.
// That was the theory; on the board the votes did not change with the facing,
// so they are only logged now. The gyro reading also feeds board_angle, which
// the swivel check needs.
constexpr int PB = CROP / 4, PMAX = 14;  // bins of 4 pixels; the largest shift looked for
struct {
    int16_t cols[2][PB], rows[2][PB];
    int cur = 0, have = 0, frame = 0;
    float gy_prev = 0, evidence = 0, check = 0;
    // raw: the votes as the rule above reads the gyro; sign: +1 when that
    // holds on this board, -1 when its gyro axis runs the other way. Learned
    // from the turn check, which needs no gyro (camera/msign).
    float raw = 0, sign = 1;
    int samples = 0;
    int64_t logged_at = 0, gyro_at = 0;
} M;

// A verdict from a turn: motion has to disagree for a while to undo it, and
// the motion votes so far say which way this board's gyro runs.
void trust_facing(bool back)
{
    if (fabsf(M.raw) >= 2.0f) {
        const float sign = (M.raw < 0) == back ? 1.0f : -1.0f;
        if (sign != M.sign) {
            M.sign = sign;
            ESP_LOGI(TAG, "motion facing rule: the gyro runs %s (learned from a turn)", sign > 0 ? "as assumed" : "reversed");
            save_prefs();
        }
    }
    M.raw = 0;
    M.evidence = back ? -6.0f : 6.0f;
}

void projections(const mosaico_camera_frame_t &frame, int16_t *cols, int16_t *rows)
{
    const uint32_t bpl = frame.bytes_per_line ? frame.bytes_per_line : frame.width * 2;
    const uint8_t *src = static_cast<const uint8_t *>(frame.data);
    const int ox = int((frame.width - CROP) / 2) & ~1, oy = int((frame.height - CROP) / 2) & ~1;
    int32_t c[PB] = {}, r[PB] = {};
    for (int y = 0; y < CROP; y += 4) {
        const uint8_t *line = src + size_t(oy + y) * bpl + size_t(ox) * 2 + 1;  // UYVY: Y on odd bytes
        int32_t sum = 0;
        for (int x = 0; x < PB; ++x) {
            const int v = line[x * 8];
            c[x] += v;
            sum += v;
        }
        r[y / 4] = sum;
    }
    for (int i = 0; i < PB; ++i) {
        cols[i] = int16_t(c[i] / 16);
        rows[i] = int16_t(r[i] / 16);
    }
}

// The shift (bins) of b against a: b[i] ~ a[i - shift]. Gradients, so a change
// of exposure does not count. false when the match is unclear.
bool shift_of(const int16_t *a, const int16_t *b, float *shift)
{
    int ga[PB], gb[PB];
    int texture = 0;
    for (int i = 1; i < PB - 1; ++i) {
        ga[i] = a[i + 1] - a[i - 1];
        gb[i] = b[i + 1] - b[i - 1];
        texture += abs(ga[i]);
    }
    if (texture < PB * 6) {
        return false;    // a blank wall or the dark
    }
    float cost[2 * PMAX + 1];
    int best = 0;
    float sum = 0;
    for (int s = -PMAX; s <= PMAX; ++s) {
        int64_t sad = 0;
        int n = 0;
        for (int i = 1 + PMAX; i < PB - 1 - PMAX; ++i, ++n) {
            sad += abs(gb[i] - ga[i - s]);
        }
        cost[s + PMAX] = float(sad) / float(n);
        sum += cost[s + PMAX];
        if (cost[s + PMAX] < cost[best + PMAX]) {
            best = s;
        }
    }
    const float mean = sum / float(2 * PMAX + 1), low = cost[best + PMAX];
    if (abs(best) >= PMAX || low > 0.6f * mean) {
        return false;
    }
    const float l = cost[best + PMAX - 1], r = cost[best + PMAX + 1], den = l - 2 * low + r;
    *shift = float(best) + (den > 0.001f ? 0.5f * (l - r) / den : 0.0f);
    return true;
}

void watch_motion(const mosaico_camera_frame_t &frame)
{
    if (!ppa_probed || ++M.frame % 2) {
        return;    // every other frame: bigger shifts, half the work
    }
    float gx = 0, gy = 0, gz = 0;
    if (bsp_imu_get_gyro(&gx, &gy, &gz) != ESP_OK) {
        return;
    }
    const int64_t at = esp_timer_get_time();
    if (M.gyro_at && at - M.gyro_at < 500000) {
        board_angle += sqrtf(gx * gx + gy * gy + gz * gz) * float(at - M.gyro_at) * 1e-6f;
    }
    M.gyro_at = at;
    const int next = M.cur ^ 1;
    projections(frame, M.cols[next], M.rows[next]);
    const float rate = 0.5f * (gy + M.gy_prev);  // over the two frames
    M.gy_prev = gy;
    const bool had = M.have;
    M.have = 1;
    const int prev = M.cur;
    M.cur = next;
    if (!had || fabsf(rate) < 6.0f || fabsf(rate) > 160.0f) {
        return;    // still, or a swing too fast to match
    }
    float sx, sy;
    if (!shift_of(M.cols[prev], M.cols[next], &sx) || !shift_of(M.rows[prev], M.rows[next], &sy)) {
        return;
    }
    // The shift as the front camera would show it, upright at rotation 0.
    const Mat &u = G.upright;
    const float fx = u.a * sx + u.b * sy, fy = u.c * sx + u.d * sy;
    if (fabsf(fx) < 0.4f) {
        return;
    }
    const float weight = std::min(fabsf(rate) / 25.0f, 1.0f) * std::min(fabsf(fx) / 1.5f, 1.0f);
    const float vote = (fx > 0) == (rate > 0) ? weight : -weight;  // + the person, - away (as assumed)
    M.raw = M.raw * 0.94f + vote;
    M.evidence = M.evidence * 0.94f + vote * M.sign;
    M.check = M.check * 0.94f + ((fy > 0) == (gx > 0) ? weight : -weight);  // (logged only: the other axis)
    ++M.samples;
    const int64_t now = esp_timer_get_time();
    if (now - M.logged_at > 1000000) {
        M.logged_at = now;
        ESP_LOGI(TAG, "facing by motion: evidence %+.1f (raw %+.1f, rule %+.0f, other axis %+.1f), turn %+.0f dps, shift %+.1f,%+.1f",
                 double(M.evidence), double(M.raw), double(M.sign), double(M.check), double(rate), double(fx), double(fy));
    }
    // Logged only. On the board the votes came out the same with the module
    // facing either way (it cannot tell front from back), and acting on them
    // undid every correct swivel; the swivel, a turn and the button decide.
}

// ---- the module swivelling ------------------------------------------------------------------
// Turning the module to the other side while the board stays nearly still
// swaps a steady picture for another steady but unrelated one: the facing
// flips at once (on the board it caught every turn of the module). A UI turn
// confirms it; the front/rear button corrects it.
int64_t swivel_quiet_until = 0;

// The newest thumbnail aged between lo and hi microseconds, or null.
const Thumb *thumb_aged(int64_t now, int64_t lo, int64_t hi)
{
    for (int i = 0; i < F.count; ++i) {
        const Thumb &t = F.ring[(F.head + RING - 1 - i) % RING];
        const int64_t age = now - t.at;
        if (age > hi) {
            break;
        }
        if (age >= lo) {
            return &t;
        }
    }
    return nullptr;
}

float spread(const Thumb &t)  // standard deviation of the luma: low for a covered lens or the dark
{
    double s = 0, ss = 0;
    for (int i = 0; i < TN * TN; ++i) {
        s += t.px[i], ss += double(t.px[i]) * t.px[i];
    }
    const double mean = s / (TN * TN);
    return float(sqrt(std::max(0.0, ss / (TN * TN) - mean * mean)));
}

void watch_swivel()
{
    static int seen = -1;
    static int64_t logged = 0;
    if (F.count < 12 || seen == F.head) {
        return;    // once per new thumbnail
    }
    seen = F.head;
    const Thumb &now_t = F.ring[(F.head + RING - 1) % RING];
    const int64_t now = now_t.at;
    if (now < swivel_quiet_until || F.turned_at) {
        return;
    }
    const Thumb *after = thumb_aged(now, 200000, 450000), *before = thumb_aged(now, 900000, 1300000),
                 *earlier = thumb_aged(now, 1500000, 2000000);
    if (!after || !before || !earlier) {
        return;
    }
    const float across = match(*before, now_t, IDENTITY);
    if (across > 0.3f) {
        return;    // the same scene: nothing to look at
    }
    const float turned = now_t.angle - earlier->angle;
    const float steady_after = match(*after, now_t, IDENTITY), steady_before = match(*earlier, *before, IDENTITY);
    const float tex_now = spread(now_t), tex_before = spread(*before);
    const char *miss = turned > 45.0f                              ? "the board turned"
                       : steady_after < 0.45f || steady_before < 0.45f ? "not steady"
                       : across > 0.2f                             ? "too similar"
                       : tex_now < 10.0f || tex_before < 10.0f     ? "too dark or covered"
                       : nullptr;
    if (miss) {
        if (now - logged > 1000000) {
            logged = now;
            ESP_LOGI(TAG, "swivel? no, %s (across %.2f, steady %.2f/%.2f, board %.0f deg, texture %.0f/%.0f)", miss,
                     double(across), double(steady_before), double(steady_after), double(turned), double(tex_before),
                     double(tex_now));
        }
        return;
    }
    G.back = !G.back;
    G.facing_known = true;  // the automatic signal (the button corrects it)
    M.raw = 0;
    swivel_quiet_until = now + 2500000;
    note_facing("swivel");
    ESP_LOGI(TAG, "swivel: the camera now faces %s (across %.2f, steady %.2f/%.2f, board %.0f deg)",
             G.back ? "away" : "the person", double(across), double(steady_before), double(steady_after), double(turned));
    save_prefs();
    facing_toast = true;
    G.ui_dirty = true;
}

esp_err_t convert(const mosaico_camera_frame_t &frame, uint16_t *out)
{
    if (frame.pixel_format != V4L2_PIX_FMT_UYVY || frame.width < CROP || frame.height < CROP) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t bpl = frame.bytes_per_line ? frame.bytes_per_line : frame.width * 2;
    esp_cache_msync(const_cast<void *>(frame.data), frame.size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_cache_msync(out, FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t op = {};
    op.in.buffer = frame.data;
    op.in.pic_w = bpl / 2;
    op.in.pic_h = frame.height;
    op.in.block_w = CROP;
    op.in.block_h = CROP;
    op.in.block_offset_x = ((frame.width - CROP) / 2) & ~1u;
    op.in.block_offset_y = ((frame.height - CROP) / 2) & ~1u;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY;
    op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = out;
    op.out.buffer_size = FRAME_BYTES;
    op.out.pic_w = W;
    op.out.pic_h = H;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    const uint8_t setting = ppa_setting();
    op.rotation_angle = ppa_srm_rotation_angle_t(setting & PPA_ANGLE);
    op.scale_x = float(W) / CROP;
    op.scale_y = float(H) / CROP;
    op.mirror_x = setting & PPA_MX;
    op.mirror_y = setting & PPA_MY;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t err = ppa_do_scale_rotate_mirror(G.ppa, &op);
    esp_cache_msync(out, FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (err == ESP_OK) {
        apply_filter(out, Filter(G.filter));
    }
    return err;
}

// A software restart (an install, Settings) keeps the camera board powered,
// and the sensor can come back without streaming. Pulse its reset line and
// release power-down before every open so it starts from scratch.
void reset_sensor()
{
    gpio_config_t io = {};
    io.pin_bit_mask = BIT64(GPIO_NUM_53) | BIT64(GPIO_NUM_48);
    io.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&io) != ESP_OK) {
        return;
    }
    gpio_set_level(GPIO_NUM_48, 0);  // power-down released
    gpio_set_level(GPIO_NUM_53, 0);  // reset asserted
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(GPIO_NUM_53, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
}

void close_camera()
{
    if (!G.camera) {
        return;
    }
    G.torch_lit = false;
    if (G.streaming) {
        mosaico_camera_stop_stream(G.camera);
    }
    G.streaming = false;
    mosaico_camera_close(G.camera);
    mosaico_camera_del(G.camera);
    G.camera = nullptr;
}

bool open_camera()
{
    reset_sensor();
    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    esp_err_t err = mosaico_camera_new(&config, &G.camera);
    if (err == ESP_OK) {
        err = mosaico_camera_open(G.camera);
    }
    if (err != ESP_OK) {
        if (G.camera) {
            mosaico_camera_del(G.camera);
        }
        G.camera = nullptr;
        G.present = false;
        G.probed = true;
        set_status("No camera module");
        return false;
    }
    G.present = true;
    G.probed = true;
    G.facing_known = false;  // the swivel may have turned while closed
    M.have = 0;
    M.evidence = M.check = M.raw = 0;
    M.gyro_at = 0;
    mosaico_camera_info_t info = {};
    if (mosaico_camera_get_info(G.camera, &info) == ESP_OK) {
        ESP_LOGI(TAG, "camera %" PRIu32 "x%" PRIu32 " @%" PRIu32 " fps", info.width, info.height, info.frame_rate);
        // The OV3640 (1024x768) is mounted as the BSP example expects (270
        // degrees, vertical mirror); the SC101IOT (1280x720) a quarter turn on.
        if (!(info.width == 1280 && info.height == 720)) {
            G.fallback = 3 | PPA_MY;
            if (ppa_probed) {
                G.upright = ppa_mats[3 | PPA_MY];
            }
        }
    }
    return true;
}

void flush_pending();

// Streaming needs the DMA channel the NAND's bus holds: storage pauses while
// the camera streams and comes back (with the photos taken meanwhile) after.
void stream(bool on)
{
    if (!G.camera || G.streaming == on) {
        return;
    }
    if (on) {
        mosaico_storage_pause(true);
    }
    if (on ? mosaico_camera_start_stream(G.camera) == ESP_OK : mosaico_camera_stop_stream(G.camera) == ESP_OK) {
        G.streaming = on;
    }
    set_status(G.streaming ? "" : "Paused");
    if (!G.streaming && G.stored) {
        mosaico_storage_pause(false);
        if (mosaico_storage_mount()) {
            flush_pending();
        }
    }
}

void show_frame(int index)
{
    if (!G.active) {  // streaming for Swifty with the Camera screen hidden: nothing to draw
        G.shown = index;
        return;
    }
    if (!bsp_display_lock(200)) {
        return;
    }
    G.shown = index;
    if (G.viewfinder && G.gallery < 0) {
        lv_image_set_src(G.viewfinder, &G.dsc[index]);
        lv_obj_invalidate(G.viewfinder);
    }
    bsp_display_unlock();
}

// The latest photo as a small square for the thumbnail button.
void make_thumb(const uint16_t *rgb)
{
    for (int y = 0; y < THUMB; ++y)
        for (int x = 0; x < THUMB; ++x) {
            G.thumb_px[y * THUMB + x] = rgb[(y * H / THUMB) * W + x * W / THUMB];
        }
}

void store_photo(const uint16_t *rgb, bool keep)
{
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = W;
    cfg.height = H;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = QUALITY;
    jpeg_enc_handle_t enc = nullptr;
    int size = 0;
    if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
        return;
    }
    const jpeg_error_t r = jpeg_enc_process(enc, (uint8_t *)rgb, int(FRAME_BYTES), G.jpeg, int(MAX_JPEG), &size);
    jpeg_enc_close(enc);
    if (r != JPEG_ERR_OK || size <= 0) {
        ESP_LOGW(TAG, "JPEG encode failed: %d", int(r));
        return;
    }
    const time_t now = time(nullptr);
    const uint32_t at = now > 1700000000 ? uint32_t(now) : 0;
    if (G.stored && !mosaico_storage_ready()) {
        // The NAND is paused while the camera streams: saved when it stops.
    } else if (G.stored && mosaico_storage_free_bytes() < KEEP_FREE + size_t(size)) {
        ESP_LOGW(TAG, "the NAND is nearly full; keeping the photo in memory");
        set_status("Storage full");
    } else if (G.stored) {  // on the NAND
        char path[64];
        snprintf(path, sizeof(path), "%s/IMG_%05u.jpg", PHOTO_DIR, G.next_number);
        FILE *f = fopen(path, "wb");
        const bool ok = f && fwrite(G.jpeg, 1, size_t(size), f) == size_t(size);
        if (f) {
            fclose(f);
        }
        if (ok) {
            std::lock_guard<std::mutex> guard(G.lock);
            ++G.next_number;
            G.photos.push_back({nullptr, size_t(size), at, path, std::string(), true});
            ESP_LOGI(TAG, "photo %u saved: %s, %d bytes", unsigned(G.photos.size()), path, size);
            return;
        }
        unlink(path);
        ESP_LOGW(TAG, "could not save %s; keeping the photo in memory", path);
    }
    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(size_t(size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) {
        return;
    }
    memcpy(copy, G.jpeg, size_t(size));
    std::lock_guard<std::mutex> guard(G.lock);
    // Over the memory budget: drop the oldest temporary photos first, and
    // only then Camera photos still waiting for the NAS.
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < G.photos.size() && (G.photos.size() >= MAX_PHOTOS || G.photo_bytes + size_t(size) > MAX_PHOTO_BYTES);) {
            Photo &old = G.photos[i];
            if (old.jpeg && old.path.empty() && old.nas.empty() && (pass || !old.keep)) {
                if (old.keep) {
                    ESP_LOGW(TAG, "memory full: dropping a photo the NAS never got");
                }
                G.photo_bytes -= old.size;
                heap_caps_free(old.jpeg);
                G.photos.erase(G.photos.begin() + long(i));
            } else {
                ++i;
            }
        }
    G.photos.push_back({copy, size_t(size), at, std::string(), std::string(), keep});
    G.photo_bytes += size_t(size);
    G.stored_count = G.stored_count + 1;
    ++photos_revision;
    ESP_LOGI(TAG, "photo %u: %d bytes", unsigned(G.photos.size()), size);
    if (uploader) {
        xTaskNotifyGive(uploader);
    }
}

// A photo's JPEG, from memory, its file or the NAS (takes G.lock itself, and
// not across the network).
bool photo_bytes(int index, std::vector<uint8_t> &out)
{
    std::string nas;
    {
        std::lock_guard<std::mutex> guard(G.lock);
        if (index < 0 || index >= int(G.photos.size())) {
            return false;
        }
        const Photo &p = G.photos[size_t(index)];
        if (p.jpeg) {
            out.assign(p.jpeg, p.jpeg + p.size);
            return true;
        }
        if (!p.path.empty()) {
            FILE *f = fopen(p.path.c_str(), "rb");
            if (!f) {
                return false;
            }
            out.resize(p.size);
            const bool ok = fread(out.data(), 1, p.size, f) == p.size;
            fclose(f);
            return ok;
        }
        nas = p.nas;
    }
    uint8_t *data = nullptr;
    size_t size = 0;
    if (nas.empty() || !files_nas_get(nas.c_str(), &data, &size)) {
        return false;
    }
    out.assign(data, data + size);
    heap_caps_free(data);
    return true;
}

// ---- the NAS: list at start, then upload each new photo ------------------------------------
void nas_thumb(int index)
{
    std::vector<uint8_t> jpeg;
    if (!photo_bytes(index, jpeg)) {
        return;
    }
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return;
    }
    jpeg_dec_io_t io = {};
    io.inbuf = jpeg.data();
    io.inbuf_len = int(jpeg.size());
    jpeg_dec_header_info_t header = {};
    int outlen = 0;
    auto *px = static_cast<uint16_t *>(jpeg_calloc_align(FRAME_BYTES, 16));
    if (px && jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK && header.width == W && header.height == H &&
            jpeg_dec_get_outbuf_len(dec, &outlen) == JPEG_ERR_OK && size_t(outlen) <= FRAME_BYTES) {
        io.outbuf = reinterpret_cast<uint8_t *>(px);
        if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
            make_thumb(px);
            G.ui_dirty = true;
        }
    }
    if (px) {
        jpeg_free_align(px);
    }
    jpeg_dec_close(dec);
}

void upload_task(void *)
{
    bool first = true;
    for (;;) {
        // A new photo or the Gallery opening wakes it at once; the first pass runs now.
        ulTaskNotifyTake(pdTRUE, first ? 0 : pdMS_TO_TICKS(60000));
        first = false;
        if (!files_nas_ready()) {  // no share set up (yet)
            nas_list_state = -2;
            continue;
        }
        if (relist_wanted.exchange(false)) {
            static files_photo_t found[200];
            nas_list_state = 0;
            const int n = files_nas_list(found, 200);
            if (n < 0) {  // unreachable: try again later
                nas_list_state = -1;
                relist_wanted = true;
                ++photos_revision;
                continue;
            }
            std::sort(found, found + n, [](const files_photo_t &a, const files_photo_t &b) {
                return strcmp(a.name, b.name) < 0;
            });
            unsigned top = 0;
            for (int i = 0; i < n; ++i) {
                unsigned num = 0;
                if (sscanf(found[i].name, "IMG_%u.jpg", &num) == 1) {
                    top = std::max(top, num);
                }
            }
            int added = 0;
            bool only_nas;
            {
                std::lock_guard<std::mutex> guard(G.lock);
                // Photos put there by something else, or seen for the first time.
                std::vector<Photo> unseen;
                for (int i = 0; i < n; ++i) {
                    bool known = false;
                    for (const Photo &p : G.photos)
                        if (p.nas == found[i].name) {
                            known = true;
                        }
                    if (!known)
                        unseen.push_back({nullptr, found[i].size, found[i].mtime > 1700000000 ? found[i].mtime : 0,
                                          std::string(), found[i].name, true});
                }
                added = int(unseen.size());
                G.photos.insert(G.photos.begin(), unseen.begin(), unseen.end());
                // Oldest first by time where every photo has one (the Gallery shows newest first).
                bool timed = true;
                for (const Photo &p : G.photos)
                    if (!p.at) {
                        timed = false;
                    }
                if (timed && added)
                    std::stable_sort(G.photos.begin(), G.photos.end(), [](const Photo & a, const Photo & b) {
                    return a.at < b.at;
                });
                G.next_number = std::max(G.next_number, top + 1);
                only_nas = true;
                for (const Photo &p : G.photos)
                    if (p.jpeg || !p.path.empty()) {
                        only_nas = false;
                    }
                nas_list_state = 1;
                ++photos_revision;
            }
            ESP_LOGI(TAG, "%d photos on the NAS (%d new to the list)", n, added);
            if (n && added && only_nas) {
                nas_thumb(int(G.photos.size()) - 1);
            }
        }
        // Upload what is new, oldest first.
        for (;;) {
            int index = -1;
            std::vector<uint8_t> jpeg;
            const uint8_t *which = nullptr;  // the photo, by its memory (never read after this)
            char name[32];
            {
                std::lock_guard<std::mutex> guard(G.lock);
                for (size_t i = 0; i < G.photos.size(); ++i)
                    if (G.photos[i].keep && G.photos[i].jpeg && G.photos[i].nas.empty() && G.photos[i].path.empty()) {
                        index = int(i);
                        jpeg.assign(G.photos[i].jpeg, G.photos[i].jpeg + G.photos[i].size);
                        which = G.photos[i].jpeg;
                        snprintf(name, sizeof(name), "IMG_%05u.jpg", G.next_number++);
                        break;
                    }
            }
            if (index < 0) {
                break;
            }
            if (!files_nas_put(name, jpeg.data(), jpeg.size())) {
                std::lock_guard<std::mutex> guard(G.lock);
                --G.next_number;
                break;  // try again on the next photo or in 30 s
            }
            std::lock_guard<std::mutex> guard(G.lock);
            // The list may have changed meanwhile: find it by its memory.
            for (Photo &p : G.photos)
                if (p.jpeg == which && p.nas.empty()) {
                    p.nas = name;
                    break;
                }
            // Keep only the newest few uploaded photos in memory as well.
            int kept = 0;
            for (size_t i = G.photos.size(); i-- > 0;) {
                Photo &p = G.photos[i];
                if (!p.jpeg || p.nas.empty()) {
                    continue;
                }
                if (++kept > KEEP_IN_RAM) {
                    G.photo_bytes -= p.size;
                    heap_caps_free(p.jpeg);
                    p.jpeg = nullptr;
                }
            }
        }
    }
}

// The photos saved on the NAND, oldest first; the newest becomes the thumbnail.
void load_photos()
{
    if (!mosaico_storage_mount() || !mosaico_storage_writable()) {
        ESP_LOGI(TAG, "photos stay in memory: %s", mosaico_storage_status());
        return;
    }
    mkdir(PHOTO_DIR, 0775);
    DIR *d = opendir(PHOTO_DIR);
    if (!d) {
        return;
    }
    std::vector<Photo> found;
    unsigned top = 0;
    while (dirent *e = readdir(d)) {
        unsigned n = 0;
        if (sscanf(e->d_name, "IMG_%u.jpg", &n) != 1) {
            continue;
        }
        const std::string path = std::string(PHOTO_DIR) + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) || st.st_size <= 0) {
            continue;
        }
        found.push_back({nullptr, size_t(st.st_size), uint32_t(st.st_mtime > 1700000000 ? st.st_mtime : 0), path, std::string(), true});
        top = std::max(top, n);
    }
    closedir(d);
    std::sort(found.begin(), found.end(), [](const Photo & a, const Photo & b) {
        return a.path < b.path;
    });
    {
        std::lock_guard<std::mutex> guard(G.lock);
        G.photos.insert(G.photos.begin(), found.begin(), found.end());
        G.next_number = top + 1;
        G.stored = true;
    }
    ESP_LOGI(TAG, "%u photos on the NAND (%s)", unsigned(found.size()), mosaico_storage_status());
    if (found.empty()) {
        return;
    }
    // The newest one's thumbnail.
    std::vector<uint8_t> jpeg;
    {
        std::lock_guard<std::mutex> guard(G.lock);
        if (!G.photos.empty() && G.photos.back().jpeg) {
            jpeg.assign(G.photos.back().jpeg, G.photos.back().jpeg + G.photos.back().size);
        } else if (!G.photos.empty() && !G.photos.back().path.empty()) {
            FILE *f = fopen(G.photos.back().path.c_str(), "rb");
            if (!f) {
                return;
            }
            jpeg.resize(G.photos.back().size);
            const bool ok = fread(jpeg.data(), 1, jpeg.size(), f) == jpeg.size();
            fclose(f);
            if (!ok) {
                return;
            }
        } else {
            return;
        }
    }
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return;
    }
    jpeg_dec_io_t io = {};
    io.inbuf = jpeg.data();
    io.inbuf_len = int(jpeg.size());
    jpeg_dec_header_info_t header = {};
    int outlen = 0;
    if (jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK && header.width == W && header.height == H &&
            jpeg_dec_get_outbuf_len(dec, &outlen) == JPEG_ERR_OK && size_t(outlen) <= FRAME_BYTES) {
        io.outbuf = reinterpret_cast<uint8_t *>(G.view);
        if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
            make_thumb(G.view);
            G.ui_dirty = true;
        }
    }
    jpeg_dec_close(dec);
}

void (*brightness_hook)(int percent) = nullptr;
void (*gallery_hook)(void) = nullptr;

// Facing the person, the flash is the screen: white at full brightness (the
// module's LED would shine in their eyes), as phones do.
void screen_flash(bool on)
{
    if (bsp_display_lock(500)) {
        lv_anim_delete(G.flash_overlay, nullptr);
        lv_obj_set_style_bg_opa(G.flash_overlay, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (on) {
            lv_obj_move_foreground(G.flash_overlay);
        }
        bsp_display_unlock();
    }
    if (brightness_hook) {
        brightness_hook(on ? 100 : -1);
    }
}

void capture_now()
{
    mosaico_camera_flash_state_t flash = {};
    bool lit = false;
    // The LED only when the camera is known to face away; facing the person,
    // or not known yet, the screen is the flash (never the LED in their eyes).
    const bool pulse = G.flash_mode == 1 && G.back && G.facing_known;
    const bool screen = G.flash_mode == 1 && !pulse && G.active;
    if (screen) {
        screen_flash(true);
        // Exposure settles on the lit face while the white frames go out.
        const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(FLASH_LEAD_MS);
        while (xTaskGetTickCount() < until) {
            mosaico_camera_frame_t lead = {};
            if (mosaico_camera_get_frame(G.camera, &lead) != ESP_OK) {
                break;
            }
            mosaico_camera_return_frame(G.camera, &lead);
        }
    }
    if (pulse) {
        mosaico_camera_prepare_flash_capture(G.camera, &flash);  // exposure lock (OV3640 only)
        lit = mosaico_camera_flash_trigger(G.camera) == ESP_OK;
        // Let auto-exposure settle on the lit scene before the photo frame.
        const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(FLASH_LEAD_MS);
        // Each lead frame's mean luma is logged, to see the light arrive.
        char trace[160];
        int used = 0;
        for (uint32_t n = 0; lit && (n < FLASH_LEAD_FRAMES || xTaskGetTickCount() < until); ++n) {
            mosaico_camera_frame_t lead = {};
            if (mosaico_camera_get_frame(G.camera, &lead) != ESP_OK) {
                break;
            }
            esp_cache_msync(const_cast<void *>(lead.data), lead.size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            const uint8_t *px = static_cast<const uint8_t *>(lead.data);
            uint32_t sum = 0, count = 0;
            for (size_t i = 1; i < lead.size; i += 2 * 97, ++count) {
                sum += px[i];
            }
            mosaico_camera_return_frame(G.camera, &lead);
            if (used < int(sizeof(trace)) - 8) {
                used += snprintf(trace + used, sizeof(trace) - used, " %u", unsigned(count ? sum / count : 0));
            }
        }
        ESP_LOGI(TAG, "flash lead luma:%s", trace);
    }
    mosaico_camera_frame_t frame = {};
    if (mosaico_camera_get_frame(G.camera, &frame) == ESP_OK) {
        uint16_t *out = G.frames[G.shown ^ 1];
        const esp_err_t err = convert(frame, out);
        mosaico_camera_return_frame(G.camera, &frame);
        if (lit) {
            mosaico_camera_flash_stop(G.camera);
        }
        lit = false;
        if (screen) {
            screen_flash(false);
        }
        if (err == ESP_OK) {
            uint64_t sum = 0;  // mean luma, for the test RPC
            for (size_t i = 0; i < size_t(W) * H; i += 7) {
                const uint16_t v = out[i];
                sum += ((v >> 11) * 8 * 77 + ((v >> 5) & 63) * 4 * 150 + (v & 31) * 8 * 29) >> 8;
            }
            G.last_luma = int(sum / (size_t(W) * H / 7));
            show_frame(G.shown ^ 1);
            make_thumb(out);
            store_photo(out, G.capture_keep);
            G.capture_keep = false;
            G.captured = true;
            G.ui_dirty = true;
        }
    }
    if (lit) {
        mosaico_camera_flash_stop(G.camera);
    }
    if (screen) {
        screen_flash(false);    // (again, if the frame failed)
    }
    if (pulse) {
        mosaico_camera_restore_flash_capture(G.camera, &flash);
    }
}

void set_torch(bool on)
{
    if (!G.camera || G.torch_lit == on) {
        return;
    }
    const esp_err_t err = on ? mosaico_camera_flash_trigger(G.camera) : mosaico_camera_flash_stop(G.camera);
    if (err == ESP_OK) {
        G.torch_lit = on;
    } else {
        ESP_LOGW(TAG, "torch %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
    }
}

// Photos taken while the NAND was paused: write them out now.
void flush_pending()
{
    std::lock_guard<std::mutex> guard(G.lock);
    for (Photo &p : G.photos) {
        if (!p.jpeg || !p.path.empty()) {
            continue;
        }
        if (mosaico_storage_free_bytes() < KEEP_FREE + p.size) {
            break;
        }
        char path[64];
        snprintf(path, sizeof(path), "%s/IMG_%05u.jpg", PHOTO_DIR, G.next_number);
        FILE *f = fopen(path, "wb");
        const bool ok = f && fwrite(p.jpeg, 1, p.size, f) == p.size;
        if (f) {
            fclose(f);
        }
        if (!ok) {
            unlink(path);
            ESP_LOGW(TAG, "could not save %s", path);
            break;
        }
        ++G.next_number;
        G.photo_bytes -= p.size;
        heap_caps_free(p.jpeg);
        p.jpeg = nullptr;
        p.path = path;
        ESP_LOGI(TAG, "saved %s (%u bytes)", path, unsigned(p.size));
    }
}

// ---- USB webcam ---------------------------------------------------------------------------
// The size the computer chose and the board's up come from the shell's view():
// a centred block of the sensor, turned, mirrored and scaled by the PPA in one
// pass to exactly that size, then the hardware JPEG encoder. The sink copies it.
constexpr int WC_MAX_W = 1280, WC_MAX_H = 720;

// How a frame size comes out of the sensor for one view.
struct WebcamPlan {
    int w = 0, h = 0, rotation = -1;
    bool back = false, flip = false;
    uint32_t fw = 0, fh = 0;      // the sensor picture
    uint8_t setting = 0;          // PPA angle and mirrors
    uint32_t bw = 0, bh = 0, ox = 0, oy = 0;  // the block, in sensor pixels
    int k = 16;                   // scale k / 16
};

// The PPA's own rounding of a block side b at scale k / 16 (ppa_srm.c).
uint32_t ppa_scaled(uint32_t b, int k)
{
    return uint32_t(k / 16) * b + uint32_t(k % 16) * b / 16;
}

// A block side that the PPA scales to exactly out at k / 16, at most max (even
// if asked: UYVY's x); 0 if there is none.
uint32_t block_side(int out, int k, uint32_t max, bool even)
{
    const int c = (out * 16 + k - 1) / k;
    for (int b = c - 1; b <= c + 1; ++b)
        if (b > 0 && uint32_t(b) <= max && !(even && (b & 1)) && ppa_scaled(b, k) == uint32_t(out)) {
            return b;
        }
    return 0;
}

// The PPA setting from want_mat (upright for the rotation), then the smallest
// scale, i.e. the most of the sensor, whose block fits: at UI rotation 0 or 180
// the sensor lies a quarter turned, so 1280 x 720 takes a 683 x 384 block
// enlarged 1.875 times; at 90 or 270 the whole sensor, as it is.
bool plan_webcam(WebcamPlan &p)
{
    const Mat want = want_mat(p.rotation, p.back, p.flip);
    int setting = -1;
    for (int i = 0; i < 16 && setting < 0; ++i)
        if (ppa_mats[i] == want) {
            setting = i;
        }
    if (setting < 0) {
        return false;
    }
    const bool quarter = setting & 1;  // 90 or 270: the frame's width runs along the sensor's y
    for (int k = 1; k < 16 * 16; ++k) {
        const uint32_t bx = block_side(quarter ? p.h : p.w, k, p.fw, true);
        const uint32_t by = block_side(quarter ? p.w : p.h, k, p.fh, false);
        if (!bx || !by) {
            continue;
        }
        p.setting = uint8_t(setting);
        p.bw = bx, p.bh = by, p.k = k;
        p.ox = ((p.fw - bx) / 2) & ~1u;
        p.oy = (p.fh - by) / 2;
        return true;
    }
    return false;
}

// Time per webcam frame, by stage (camera_app_webcam_stats).
struct {
    int64_t ppa = 0, jpeg = 0, total = 0, worst = 0;
    int n = 0;
    uint32_t bytes = 0;
} WS;

// One picture between the PPA and the encoder, one after the other on the
// capture task: run side by side (two pictures, an encoder task) both engines
// slowed down (they share the memory bandwidth) and 720p fell from ~24 to ~17 fps.
struct WebcamSlot {
    void *buf = nullptr;
    size_t size = 0;
    int w = 0, h = 0;
    bool yuv = false;
    int64_t ppa_us = 0;
};
WebcamSlot wc_slot;

void webcam_encode(const WebcamSlot &s)
{
    const int64_t t1 = esp_timer_get_time();
    jpeg_encode_cfg_t enc = {};
    enc.width = s.w;
    enc.height = s.h;
    enc.src_type = s.yuv ? JPEG_ENCODE_IN_FORMAT_YUV420 : JPEG_ENCODE_IN_FORMAT_RGB565;
    enc.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    enc.image_quality = s.w * s.h > 640 * 480 ? 72 : 80;  // 720p: keeps a frame near 100 KB for the USB link
    uint32_t size = 0;
    if (jpeg_encoder_process(G.wc_encoder, &enc, static_cast<const uint8_t *>(s.buf),
                             uint32_t(s.yuv ? s.w * s.h * 3 / 2 : s.w * s.h * 2), G.wc_jpeg, G.wc_jpeg_size,
                             &size) != ESP_OK || !size) {
        return;
    }
    const int64_t t2 = esp_timer_get_time();
    // The encoder wrote it by DMA; the sink reads it with the CPU.
    esp_cache_msync(G.wc_jpeg, G.wc_jpeg_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (void (*sink)(const unsigned char *, size_t) = G.webcam_sink) {
        sink(G.wc_jpeg, size);
    }
    const int64_t t3 = esp_timer_get_time();
    WS.ppa += s.ppa_us, WS.jpeg += t2 - t1, WS.total += s.ppa_us + (t3 - t1);
    WS.worst = std::max(WS.worst, s.ppa_us + (t3 - t1));
    WS.bytes = size;
    ++WS.n;
}

bool webcam_setup()
{
    jpeg_encode_engine_cfg_t engine = {};
    engine.timeout_ms = 200;
    if (jpeg_new_encoder_engine(&engine, &G.wc_encoder) != ESP_OK) {
        ESP_LOGW(TAG, "webcam: no JPEG encoder");
        return false;
    }
    jpeg_encode_memory_alloc_cfg_t in = {}, out = {};
    in.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    out.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    wc_slot.buf = jpeg_alloc_encoder_mem(WC_MAX_W * WC_MAX_H * 2, &in, &wc_slot.size);
    G.wc_jpeg = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(480 * 1024, &out, &G.wc_jpeg_size));
    return wc_slot.buf && G.wc_jpeg;
}

void webcam_frame(const mosaico_camera_frame_t &frame)
{
    void (*sink)(const unsigned char *, size_t) = G.webcam_sink;
    const camera_webcam_view_t view = G.webcam_view;
    if (!sink || !view || !ppa_probed || frame.pixel_format != V4L2_PIX_FMT_UYVY) {
        return;
    }
    int w = 0, h = 0, rotation = 0;
    view(&w, &h, &rotation);
    if (w <= 0 || h <= 0 || w > WC_MAX_W || h > WC_MAX_H) {
        return;    // nobody watching
    }
    static bool ready = false, failed = false;
    if (!ready && !failed) {
        ready = webcam_setup();
        failed = !ready;
        if (failed) {
            ESP_LOGW(TAG, "webcam: no memory for frames");
        }
    }
    if (!ready) {
        return;
    }
    const uint32_t bpl = frame.bytes_per_line ? frame.bytes_per_line : frame.width * 2;
    // The true view (others see you as the front camera does; the rear is a
    // window), unless the flip button mirrored it: want_mat's flip is the
    // front's true view and the rear's mirror.
    const bool flip = G.back ? G.mirror_back : !G.mirror;
    static WebcamPlan plan;
    if (plan.w != w || plan.h != h || plan.rotation != rotation || plan.back != G.back || plan.flip != flip ||
            plan.fw != bpl / 2 || plan.fh != frame.height) {
        plan = {};
        plan.w = w, plan.h = h, plan.rotation = rotation, plan.back = G.back, plan.flip = flip;
        plan.fw = bpl / 2, plan.fh = frame.height;
        if (!plan_webcam(plan)) {
            ESP_LOGW(TAG, "webcam: no way to make %dx%d at rotation %d", w, h, rotation);
            plan.bw = 0;
        } else {
            ESP_LOGI(TAG, "webcam %dx%d, rotation %d, %s%s: sensor block %lux%lu at %lu,%lu, scale %d/16, PPA %d", w, h,
                     rotation, G.back ? "rear" : "front", flip ? " flipped" : "", (unsigned long)plan.bw,
                     (unsigned long)plan.bh, (unsigned long)plan.ox, (unsigned long)plan.oy, plan.k, plan.setting);
        }
    }
    if (!plan.bw) {
        return;
    }
    WebcamSlot &slot = wc_slot;
    const bool yuv = G.wc_yuv;
    ppa_srm_oper_config_t op = {};
    op.in.buffer = frame.data;
    op.in.pic_w = bpl / 2;
    op.in.pic_h = frame.height;
    op.in.block_w = plan.bw;
    op.in.block_h = plan.bh;
    op.in.block_offset_x = plan.ox;
    op.in.block_offset_y = plan.oy;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY;
    op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = slot.buf;
    op.out.buffer_size = slot.size;
    op.out.pic_w = w;
    op.out.pic_h = h;
    op.out.srm_cm = yuv ? PPA_SRM_COLOR_MODE_YUV420 : PPA_SRM_COLOR_MODE_RGB565;
    // JPEG's YCbCr is full range (the sensor's is limited): the PPA converts.
    op.out.yuv_range = PPA_COLOR_RANGE_FULL;
    op.out.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.rotation_angle = ppa_srm_rotation_angle_t(plan.setting & PPA_ANGLE);
    op.scale_x = op.scale_y = float(plan.k) / 16.0f;
    op.mirror_x = plan.setting & PPA_MX;
    op.mirror_y = plan.setting & PPA_MY;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const int64_t t0 = esp_timer_get_time();
    if (ppa_do_scale_rotate_mirror(G.ppa, &op) != ESP_OK) {
        return;
    }
    slot.w = w, slot.h = h, slot.yuv = yuv;
    slot.ppa_us = esp_timer_get_time() - t0;
    webcam_encode(slot);
}

void capture_task(void *)
{
    load_photos();
    int failures = 0;
    for (;;) {
        // A photo open in the gallery stops the stream too: it frees the
        // NAND's DMA channel, so saved photos can be read.
        const bool webcam = G.webcam_sink != nullptr;
        if (G.shutdown || !(G.active || G.headless || webcam) || (G.gallery >= 0 && !G.headless && !webcam)) {
            stream(false);
            // Idle for a while (not the torch, not a photo open): close the
            // camera, which gives back the internal DMA memory its driver
            // keeps; without it the speech socket's TLS could not start.
            static TickType_t idle_since = 0;
            if (G.camera && !G.torch_only && G.gallery < 0 && !G.active && !G.headless && !webcam) {
                if (!idle_since) {
                    idle_since = xTaskGetTickCount();
                }
                if (xTaskGetTickCount() - idle_since >= pdMS_TO_TICKS(100)) {  // right away: Swifty may listen next
                    close_camera();
                    idle_since = 0;
                    ESP_LOGI(TAG, "camera closed while unused");
                }
            } else {
                idle_since = 0;
            }
            if (G.torch_only && !G.shutdown) {
                if (!G.camera && !open_camera()) {
                    G.torch_only = false;
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                set_torch(true);
            } else {
                set_torch(false);
            }
            vTaskDelay(pdMS_TO_TICKS(G.shutdown ? 1000 : 100));
            continue;
        }
        if (!G.camera && !open_camera()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        stream(true);
        if (!G.streaming) {
            // The stream did not start (no frames): reopen from a sensor reset.
            static int start_failures = 0;
            if (++start_failures >= 3) {
                start_failures = 0;
                ESP_LOGW(TAG, "camera gives no frames; resetting the sensor");
                close_camera();
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        // The flash set to always on, or the Tools torch (lit while the camera
        // streams too, e.g. for the USB webcam).
        set_torch(G.flash_mode == 2 || G.torch_only);
        if (G.capture) {
            G.capture = false;
            capture_now();
            continue;
        }
        mosaico_camera_frame_t frame = {};
        if (mosaico_camera_get_frame(G.camera, &frame) != ESP_OK) {
            if (++failures >= 3) {
                failures = 0;
                ESP_LOGW(TAG, "restarting the camera stream");
                mosaico_camera_restart(G.camera);
            }
            continue;
        }
        failures = 0;
        // A photo open in the gallery needs no viewfinder; a restart wants the
        // stream stopped now (the loop's top does it), not one more frame drawn.
        if (webcam && !G.shutdown) {
            webcam_frame(frame);
        }
        if ((G.gallery >= 0 && !G.headless) || G.shutdown || (webcam && !G.active && !G.headless)) {
            mosaico_camera_return_frame(G.camera, &frame);  // the webcam alone needs no viewfinder
            continue;
        }
        const int next = G.shown ^ 1;
        const esp_err_t err = convert(frame, G.frames[next]);
        if (err == ESP_OK) {  // after convert: the frame is fresh in the cache
            track_facing(frame);
            watch_motion(frame);
            if (ppa_probed && G.auto_facing) {
                watch_swivel();
            }
        }
        mosaico_camera_return_frame(G.camera, &frame);
        if (err == ESP_OK) {
            show_frame(next);
        }
    }
}

// ---- Mac download over ESP-Iris ------------------------------------------------------------
// List: u16 count, then per photo u32 size and u32 time. Read: u16 index and
// u32 offset in; up to the RPC body of JPEG bytes out.
esp_err_t rpc_list(const esp_iris_rpc_request_t *, uint8_t *out, size_t capacity, size_t *size, void *)
{
    std::lock_guard<std::mutex> guard(G.lock);
    const size_t n = std::min<size_t>(G.photos.size(), (capacity - 2) / 8);
    out[0] = uint8_t(n);
    out[1] = uint8_t(n >> 8);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v[2] = {uint32_t(G.photos[i].size), G.photos[i].at};
        memcpy(out + 2 + 8 * i, v, 8);
    }
    *size = 2 + 8 * n;
    return ESP_OK;
}

esp_err_t rpc_read(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    if (request->payload_size < 6) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t index;
    uint32_t offset;
    memcpy(&index, request->payload, 2);
    memcpy(&offset, request->payload + 2, 4);
    {
        std::unique_lock<std::mutex> guard(G.lock);
        if (index >= G.photos.size() || offset > G.photos[index].size) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!G.photos[index].jpeg && G.photos[index].path.empty()) {  // on the NAS
            guard.unlock();
            std::vector<uint8_t> bytes;
            if (!photo_bytes(index, bytes) || offset > bytes.size()) {
                return ESP_FAIL;
            }
            const size_t n = std::min<size_t>(capacity, bytes.size() - offset);
            memcpy(out, bytes.data() + offset, n);
            *size = n;
            return ESP_OK;
        }
    }
    std::lock_guard<std::mutex> guard(G.lock);
    if (index >= G.photos.size() || offset > G.photos[index].size) {
        return ESP_ERR_INVALID_ARG;
    }
    const Photo &p = G.photos[index];
    const size_t n = std::min<size_t>(capacity, p.size - offset);
    if (p.jpeg) {
        memcpy(out, p.jpeg + offset, n);
    } else {
        FILE *f = fopen(p.path.c_str(), "rb");
        if (!f) {
            return ESP_FAIL;
        }
        const bool ok = fseek(f, long(offset), SEEK_SET) == 0 && fread(out, 1, n, f) == n;
        fclose(f);
        if (!ok) {
            return ESP_FAIL;
        }
    }
    *size = n;
    return ESP_OK;
}

// Flash check: payload 1 lights the flash, 0 turns it off (the camera must be
// open); the reply is GPIO34's pin configuration.
esp_err_t rpc_flash(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    esp_err_t err = ESP_OK;
    if (request->payload_size >= 1) {
        if (!G.camera) {
            return ESP_ERR_INVALID_STATE;
        }
        err = request->payload[0] ? mosaico_camera_flash_trigger(G.camera) : mosaico_camera_flash_stop(G.camera);
    }
    FILE *f = fmemopen(out, capacity, "w");
    if (!f) {
        return ESP_ERR_NO_MEM;
    }
    gpio_input_enable(GPIO_NUM_34);  // read back what the pad actually does
    fprintf(f, "set: %s, pad level %d\n", esp_err_to_name(err), gpio_get_level(GPIO_NUM_34));
    gpio_dump_io_configuration(f, BIT64(GPIO_NUM_34));
    fflush(f);
    *size = std::min<size_t>(size_t(ftell(f)), capacity);
    fclose(f);
    return ESP_OK;
}

// Test shot: payload flash mode (0 off, 1 flash); takes a photo and replies
// with its mean luma (0-255), so flash and no flash can be compared.
esp_err_t rpc_shoot(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    if (!G.streaming || G.capture) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t saved = G.flash_mode;
    if (request->payload_size >= 1) {
        G.flash_mode = request->payload[0] ? 1 : 0;
    }
    G.last_luma = -1;
    G.capture = true;
    for (int i = 0; i < 60 && G.last_luma < 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    G.flash_mode = saved;
    G.ui_dirty = true;
    *size = size_t(snprintf(reinterpret_cast<char *>(out), capacity, "luma %d", G.last_luma));
    return ESP_OK;
}

// RPC 24: the facing and what decided it, as text (for checking on the board).
esp_err_t rpc_state(const esp_iris_rpc_request_t *, uint8_t *out, size_t capacity, size_t *size, void *)
{
    const int n = snprintf(reinterpret_cast<char *>(out), capacity,
                           "facing %s%s | last: %s | flip front %d rear %d | motion evidence %+.1f raw %+.1f rule %+.0f | "
                           "board turned %.0f deg | streaming %d",
                           G.back ? "rear" : "front", G.facing_known ? "" : " (unconfirmed)", G.facing_why, int(G.mirror),
                           int(G.mirror_back), double(M.evidence), double(M.raw), double(M.sign), double(board_angle),
                           int(G.streaming));
    *size = n < 0 ? 0 : std::min(capacity, size_t(n));
    return ESP_OK;
}

// ---- UI ---------------------------------------------------------------------------------------
lv_obj_t *circle_button(lv_obj_t *parent, int x, int y, int size, uint32_t bg, lv_opa_t opa = LV_OPA_COVER)
{
    lv_obj_t *b = box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, opa, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    return b;
}

void toggle_look(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? ACC.glow : 0x000000), 0);
    lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : 150, 0);
    lv_obj_set_style_line_color(lv_obj_get_child(b, 0), lv_color_hex(on ? ACC.ink : TEXT), 0);
}

void pop(void *o, int32_t v)
{
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t *>(o), v, 0);
}

void update_ui()
{
    G.ui_dirty = false;
    const bool live = G.streaming;
    lv_obj_set_style_bg_color(G.status_dot, lv_color_hex(live ? RED : G.present ? WARN : TEXT3), 0);
    // Live needs no word: the chip shows only a state to notice, or the filter.
    char status[64];
    if (live) {
        strlcpy(status, G.filter != NORMAL ? FILTER_NAMES[G.filter] : "", sizeof(status));
    } else {
        strlcpy(status, G.status, sizeof(status));
    }
    lv_label_set_text(G.status_label, status);
    lv_obj_t *chip = lv_obj_get_parent(G.status_label);
    if (status[0]) {
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);
    }
    size_t n;
    {
        std::lock_guard<std::mutex> guard(G.lock);
        n = G.photos.size();
    }
    if (n) {
        lv_obj_remove_flag(G.thumb_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(G.thumb_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (G.captured && G.pick) {  // taken for Swifty: hand it over
        G.captured = false;
        auto done = G.pick;
        G.pick = nullptr;
        done(int(n) - 1);
        return;
    }
    if (G.captured) {  // the new photo lands in the thumbnail
        G.captured = false;
        lv_obj_invalidate(G.thumb_image);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, G.thumb);
        lv_anim_set_exec_cb(&a, pop);
        lv_anim_set_values(&a, 180, 256);
        lv_anim_set_duration(&a, 320);
        lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
        lv_anim_start(&a);
    }
    toggle_look(G.flash_btn, G.flash_mode == 1);
    if (G.flash_mode == 2) {  // torch: white
        lv_obj_set_style_bg_color(G.flash_btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(G.flash_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_line_color(lv_obj_get_child(G.flash_btn, 0), lv_color_black(), 0);
    }
    toggle_look(G.mirror_btn, mirrored());
    toggle_look(G.facing_btn, G.back);
    if (facing_toast) {
        facing_toast = false;
        show_toast(G.back ? "Rear camera" : "Front camera");
    }
    toggle_look(G.filter_btn, G.filter != NORMAL || !lv_obj_has_flag(G.strip, LV_OBJ_FLAG_HIDDEN));
    for (int i = 0; i < FILTERS; ++i) {
        const bool on = i == G.filter;
        lv_obj_set_style_bg_color(G.chips[i], lv_color_hex(on ? ACC.glow : 0x000000), 0);
        lv_obj_set_style_bg_opa(G.chips[i], on ? LV_OPA_COVER : 150, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(G.chips[i], 0), lv_color_hex(on ? ACC.ink : TEXT), 0);
    }
    if (!G.present && G.probed && G.started && !live) {
        lv_obj_remove_flag(G.message, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(G.message, LV_OBJ_FLAG_HIDDEN);
    }
}

void flash_fade(void *o, int32_t v)
{
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(o), lv_opa_t(v), 0);
}

void shutter_cb(lv_event_t *)
{
    if (!G.streaming || G.capture) {
        return;
    }
    G.capture_keep = !G.pick;  // a photo picked for Swifty stays temporary
    G.capture = true;
    if (G.flash_mode == 1 && !G.back) {
        return;    // the screen flash is the capture task's
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, G.flash_overlay);
    lv_anim_set_exec_cb(&a, flash_fade);
    lv_anim_set_values(&a, 230, 0);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_delay(&a, G.flash_mode == 1 ? FLASH_LEAD_MS : 0);
    lv_anim_start(&a);
}

void save_prefs()
{
    nvs_handle_t h;
    if (nvs_open("camera", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mirror", G.mirror);
        nvs_set_u8(h, "mirrorb", G.mirror_back);
        nvs_set_u8(h, "autoface", G.auto_facing);
        nvs_set_u8(h, "msign", M.sign < 0);
        nvs_set_u8(h, "flash", G.flash_mode == 1);  // the torch is not remembered
        nvs_set_u8(h, "filter", G.filter);
        nvs_set_u8(h, "back", G.back);
        nvs_commit(h);
        nvs_close(h);
    }
}

void show_toast(const char *text);
// Off -> flash -> torch -> off.
void flash_cb(lv_event_t *)
{
    G.flash_mode = uint8_t((G.flash_mode + 1) % 3);
    save_prefs();
    static const char *const NAMES[] = {"Flash off", "Flash on", "Torch"};
    show_toast(NAMES[G.flash_mode]);
    update_ui();
}

// Flips the picture left to right (for the facing shown: each remembers its own).
void mirror_cb(lv_event_t *)
{
    volatile bool &flip = G.back ? G.mirror_back : G.mirror;
    flip = !flip;
    save_prefs();
    show_toast(flip ? "Flipped" : "Not flipped");
    update_ui();
}

// The camera faces the other way: for when the automatic guess is wrong (a
// trusted answer, which also tells the motion check which way the gyro runs).
void facing_cb(lv_event_t *)
{
    G.back = !G.back;
    G.facing_known = true;
    trust_facing(G.back);
    note_facing("button");
    save_prefs();
    facing_toast = true;
    update_ui();
}

void toast_fade(void *o, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(o), lv_opa_t(v), 0);
}

void show_toast(const char *text)
{
    lv_label_set_text(lv_obj_get_child(G.filter_toast, 0), text);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, G.filter_toast);
    lv_anim_set_exec_cb(&a, toast_fade);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_delay(&a, 700);
    lv_anim_start(&a);
    toast_fade(G.filter_toast, 255);
}

void set_filter(int f)
{
    G.filter = uint8_t((f % FILTERS + FILTERS) % FILTERS);
    save_prefs();
    show_toast(FILTER_NAMES[G.filter]);
    update_ui();
    lv_obj_scroll_to_view(G.chips[G.filter], LV_ANIM_ON);
}

void chip_cb(lv_event_t *e)
{
    set_filter(int(intptr_t(lv_event_get_user_data(e))));
}
void filter_btn_cb(lv_event_t *)
{
    if (lv_obj_has_flag(G.strip, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(G.strip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(G.strip, LV_OBJ_FLAG_HIDDEN);
    }
    update_ui();
}

// Gallery: one photo decoded full screen; swipe or the arrows to move.
void show_photo(int index)
{
    size_t total;
    std::vector<uint8_t> copy;
    uint32_t at = 0;
    if (G.gallery < 0) {
        G.gallery = index;    // stops the stream: the NAND comes back
    }
    for (int i = 0; i < 40 && G.stored && !mosaico_storage_ready(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    {
        std::lock_guard<std::mutex> guard(G.lock);
        total = G.photos.size();
        if (!total) {
            return;
        }
        index = (index % int(total) + int(total)) % int(total);
        at = G.photos[index].at;
    }
    if (!photo_bytes(index, copy)) {  // e.g. the NAS is unreachable
        G.gallery = index;
        lv_label_set_text(G.gallery_title, "This photo is on the NAS, which does not answer.");
        lv_obj_remove_flag(G.gallery_layer, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    G.gallery = index;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) == JPEG_ERR_OK) {
        jpeg_dec_io_t io = {};
        io.inbuf = copy.data();
        io.inbuf_len = int(copy.size());
        jpeg_dec_header_info_t header = {};
        int outlen = 0;
        if (jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK && jpeg_dec_get_outbuf_len(dec, &outlen) == JPEG_ERR_OK &&
                size_t(outlen) <= FRAME_BYTES) {
            io.outbuf = reinterpret_cast<uint8_t *>(G.view);
            jpeg_dec_process(dec, &io);
        }
        jpeg_dec_close(dec);
    }
    lv_image_set_src(G.photo_image, &G.view_dsc);
    lv_obj_invalidate(G.photo_image);
    char title[48];
    char when[16] = "";
    if (at) {
        const time_t t = at;
        struct tm local;
        localtime_r(&t, &local);
        strftime(when, sizeof(when), " \xC2\xB7 %H:%M", &local);
    }
    snprintf(title, sizeof(title), "%d of %u \xC2\xB7 %u KB%s", index + 1, unsigned(total), unsigned(copy.size() / 1024), when);
    lv_label_set_text(G.gallery_title, title);
    lv_obj_remove_flag(G.gallery_layer, LV_OBJ_FLAG_HIDDEN);
}

void close_gallery_cb(lv_event_t *)
{
    G.gallery = -1;
    lv_obj_add_flag(G.gallery_layer, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(G.viewfinder, &G.dsc[G.shown]);
    lv_obj_invalidate(G.viewfinder);
}
void open_gallery_cb(lv_event_t *)
{
    if (gallery_hook) {  // the Gallery app
        gallery_hook();
        return;
    }
    size_t n;
    {
        std::lock_guard<std::mutex> guard(G.lock);
        n = G.photos.size();
    }
    if (n) {
        show_photo(int(n) - 1);
    }
}
void step_cb(lv_event_t *e)
{
    show_photo(G.gallery + int(intptr_t(lv_event_get_user_data(e))));
}
// Swipe: between photos in the gallery, between filters in the viewfinder.
void swipe_cb(lv_event_t *)
{
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) {
        return;
    }
    const int step = dir == LV_DIR_LEFT ? 1 : -1;
    if (G.gallery >= 0) {
        show_photo(G.gallery + step);
    } else if (G.streaming) {
        set_filter(G.filter + step);
    }
}
void delete_cb(lv_event_t *)
{
    size_t n;
    {
        std::lock_guard<std::mutex> guard(G.lock);
        if (G.gallery < 0 || G.gallery >= int(G.photos.size())) {
            return;
        }
        Photo &p = G.photos[G.gallery];
        if (p.jpeg) {
            G.photo_bytes -= p.size;
            heap_caps_free(p.jpeg);
        }
        if (!p.path.empty()) {
            unlink(p.path.c_str());
        }
        if (!p.nas.empty()) {
            files_nas_delete(p.nas.c_str());
        }
        G.photos.erase(G.photos.begin() + G.gallery);
        n = G.photos.size();
    }
    update_ui();
    if (n) {
        show_photo(std::min(G.gallery, int(n) - 1));
    } else {
        close_gallery_cb(nullptr);
    }
}

void ui_timer(lv_timer_t *)
{
    if (G.ui_dirty) {
        update_ui();
    }
}

void build_ui(lv_obj_t *s)
{
    for (int i = 0; i < 2; ++i) {
        G.dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        G.dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        G.dsc[i].header.w = W;
        G.dsc[i].header.h = H;
        G.dsc[i].header.stride = W * 2;
        G.dsc[i].data = reinterpret_cast<const uint8_t *>(G.frames[i]);
        G.dsc[i].data_size = FRAME_BYTES;
    }
    G.view_dsc = G.dsc[0];
    G.view_dsc.data = reinterpret_cast<const uint8_t *>(G.view);
    G.thumb_dsc = G.dsc[0];
    G.thumb_dsc.header.w = THUMB;
    G.thumb_dsc.header.h = THUMB;
    G.thumb_dsc.header.stride = THUMB * 2;
    G.thumb_dsc.data = reinterpret_cast<const uint8_t *>(G.thumb_px);
    G.thumb_dsc.data_size = THUMB * THUMB * 2;

    G.viewfinder = lv_image_create(s);
    lv_obj_set_pos(G.viewfinder, 0, 0);
    lv_image_set_src(G.viewfinder, &G.dsc[0]);
    lv_obj_add_flag(G.viewfinder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(G.viewfinder, swipe_cb, LV_EVENT_GESTURE, nullptr);

    G.message = box(s, 60, 170, 360, 140, 28);
    style_glass(G.message, ACC);
    icon(G.message, "camera", 24, 22, 32, ACC.glow, 1.8f);
    label(G.message, "No camera module", 68, 26, 270, TEXT, 18, true);
    lv_obj_t *hint = label(G.message, "Plug the camera into the left slot. It is checked every two seconds.", 24, 70,
                           312, TEXT2, 15);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(hint, 48);
    lv_obj_add_flag(G.message, LV_OBJ_FLAG_HIDDEN);

    // Top, between the corners: flash, status, filters.
    G.flash_btn = circle_button(s, 76, 64, 52, 0x000000, 150);
    icon(G.flash_btn, "bulb", 13, 13, 26, TEXT, 1.9f);
    lv_obj_add_event_cb(G.flash_btn, flash_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *chip = box(s, 150, 14, 180, 40, 20);
    lv_obj_set_style_bg_color(chip, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chip, 150, 0);
    G.status_dot = dot(chip, 18, 15, 10, TEXT3);
    G.status_label = label(chip, G.status, 36, 10, 136, TEXT, 15, true);
    // Front or rear: lit while the camera faces away.
    G.facing_btn = circle_button(s, 214, 64, 52, 0x000000, 150);
    icon(G.facing_btn, "camera", 13, 13, 26, TEXT, 1.9f);
    lv_obj_add_event_cb(G.facing_btn, facing_cb, LV_EVENT_CLICKED, nullptr);
    G.filter_btn = circle_button(s, 352, 64, 52, 0x000000, 150);
    icon(G.filter_btn, "sliders", 13, 13, 26, TEXT, 1.9f);
    lv_obj_add_event_cb(G.filter_btn, filter_btn_cb, LV_EVENT_CLICKED, nullptr);

    // Filter strip above the shutter (shown with the filter button).
    G.strip = box(s, 30, 312, 420, 48);
    lv_obj_add_flag(G.strip, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_scroll_dir(G.strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(G.strip, LV_SCROLLBAR_MODE_OFF);
    int x = 0;
    for (int i = 0; i < FILTERS; ++i) {
        const int w = text_width(FILTER_NAMES[i], 15) + 34;
        G.chips[i] = box(G.strip, x, 4, w, 40, LV_RADIUS_CIRCLE);
        label(G.chips[i], FILTER_NAMES[i], 0, 10, w, TEXT, 15, true, LV_TEXT_ALIGN_CENTER);
        lv_obj_add_flag(G.chips[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(G.chips[i], chip_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(i)));
        x += w + 8;
    }
    lv_obj_add_flag(G.strip, LV_OBJ_FLAG_HIDDEN);

    // Bottom row, as the Apple camera: last photo, shutter, mirror.
    // Children sit inside the border, so the shutter is centred explicitly.
    lv_obj_t *ring = box(s, 202, 378, 76, 76, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(ring, 4, 0);
    lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_t *shutter = circle_button(ring, 0, 0, 60, 0xFFFFFF);
    lv_obj_center(shutter);
    lv_obj_add_event_cb(shutter, shutter_cb, LV_EVENT_CLICKED, nullptr);
    G.thumb = box(s, 94, 386, THUMB, THUMB, 14);
    lv_obj_set_style_bg_color(G.thumb, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(G.thumb, 150, 0);
    lv_obj_set_style_border_width(G.thumb, 2, 0);
    lv_obj_set_style_border_color(G.thumb, lv_color_white(), 0);
    lv_obj_set_style_clip_corner(G.thumb, true, 0);
    lv_obj_add_flag(G.thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(G.thumb, open_gallery_cb, LV_EVENT_CLICKED, nullptr);
    G.thumb_image = lv_image_create(G.thumb);
    lv_image_set_src(G.thumb_image, &G.thumb_dsc);
    lv_obj_center(G.thumb_image);
    lv_obj_add_flag(G.thumb_image, LV_OBJ_FLAG_HIDDEN);
    G.mirror_btn = circle_button(s, 326, 386, THUMB, 0x000000, 150);
    icon(G.mirror_btn, "history", 15, 15, 30, TEXT, 1.9f);
    lv_obj_add_event_cb(G.mirror_btn, mirror_cb, LV_EVENT_CLICKED, nullptr);

    // Filter name, briefly, when it changes.
    G.filter_toast = box(s, 165, 200, 150, 44, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(G.filter_toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(G.filter_toast, 170, 0);
    label(G.filter_toast, "", 0, 11, 150, TEXT, 17, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_opa(G.filter_toast, 0, 0);

    G.flash_overlay = box(s, 0, 0, W, H);
    lv_obj_set_style_bg_color(G.flash_overlay, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(G.flash_overlay, 0, 0);

    // Gallery: the decoded photo on black, covering the camera's controls.
    G.gallery_layer = box(s, 0, 0, W, H);
    lv_obj_set_style_bg_color(G.gallery_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(G.gallery_layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(G.gallery_layer, LV_OBJ_FLAG_CLICKABLE);  // touches stay off the viewfinder
    lv_obj_add_event_cb(G.gallery_layer, swipe_cb, LV_EVENT_GESTURE, nullptr);
    G.photo_image = lv_image_create(G.gallery_layer);
    lv_obj_set_pos(G.photo_image, 0, 0);
    lv_obj_remove_flag(G.photo_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *back = field_button(G.gallery_layer, 64, 16, 100, 44, "Back", 15, TEXT);
    lv_obj_add_event_cb(back, close_gallery_cb, LV_EVENT_CLICKED, nullptr);
    G.gallery_title = label(G.gallery_layer, "", 176, 27, 240, TEXT, 15, true);
    lv_obj_t *prev = field_button(G.gallery_layer, 70, 396, 80, 50, "\xE2\x80\xB9", 24, TEXT);
    lv_obj_add_event_cb(prev, step_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(-1)));
    lv_obj_t *del = field_button(G.gallery_layer, 170, 396, 140, 50, "Delete", 16, RED);
    lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *next = field_button(G.gallery_layer, 330, 396, 80, 50, "\xE2\x80\xBA", 24, TEXT);
    lv_obj_add_event_cb(next, step_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(1)));
    lv_obj_add_flag(G.gallery_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(ui_timer, 150, nullptr);
    update_ui();
}
}  // namespace

extern "C" void camera_app_open(lv_obj_t *screen)
{
    camera_app_start(screen);
    camera_app_set_active(true);
}

extern "C" void camera_app_start(lv_obj_t *screen)
{
    if (!G.started) {
        G.screen = screen;
        const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
        for (auto &f : G.frames) {
            f = static_cast<uint16_t *>(heap_caps_aligned_calloc(ALIGN, 1, FRAME_BYTES, caps));
        }
        G.view = static_cast<uint16_t *>(heap_caps_aligned_calloc(ALIGN, 1, FRAME_BYTES, caps));
        G.thumb_px = static_cast<uint16_t *>(heap_caps_calloc(1, THUMB * THUMB * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        G.jpeg = static_cast<uint8_t *>(heap_caps_malloc(MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ppa_client_config_t ppa = {};
        ppa.oper_type = PPA_OPERATION_SRM;
        ppa.max_pending_trans_num = 1;
        if (!G.frames[0] || !G.frames[1] || !G.view || !G.thumb_px || !G.jpeg || ppa_register_client(&ppa, &G.ppa) != ESP_OK) {
            label(screen, "Camera needs PSRAM", 40, 220, 400, RED, 20, true, LV_TEXT_ALIGN_CENTER);
            return;
        }
        build_luts();
        ppa_probed = probe_ppa();
        if (ppa_probed) {
            G.upright = ppa_mats[1 | PPA_MY];
        }
        nvs_handle_t h;
        if (nvs_open("camera", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(h, "mirror", &v) == ESP_OK) {
                G.mirror = v;
            }
            if (nvs_get_u8(h, "mirrorb", &v) == ESP_OK) {
                G.mirror_back = v;
            }
            if (nvs_get_u8(h, "autoface", &v) == ESP_OK) {
                G.auto_facing = v;
            }
            if (nvs_get_u8(h, "msign", &v) == ESP_OK) {
                M.sign = v ? -1.0f : 1.0f;
            }
            if (nvs_get_u8(h, "flash", &v) == ESP_OK) {
                G.flash_mode = v ? 1 : 0;
            }
            if (nvs_get_u8(h, "filter", &v) == ESP_OK && v < FILTERS) {
                G.filter = v;
            }
            if (nvs_get_u8(h, "back", &v) == ESP_OK) {
                G.back = v;
            }
            nvs_close(h);
        }
        build_ui(screen);
        esp_iris_rpc_register(RPC_SERVICE, RPC_LIST, rpc_list, nullptr);
        esp_iris_rpc_register(RPC_SERVICE, RPC_READ, rpc_read, nullptr);
        esp_iris_rpc_register(RPC_SERVICE, RPC_FLASH, rpc_flash, nullptr);
        esp_iris_rpc_register(RPC_SERVICE, RPC_SHOOT, rpc_shoot, nullptr);
        esp_iris_rpc_register(RPC_SERVICE, RPC_STATE, rpc_state, nullptr);
        esp_register_shutdown_handler(camera_app_shutdown);
        xTaskCreatePinnedToCore(capture_task, "camera", 8192, nullptr, 4, nullptr, 1);
        // Syncs the Camera's own photos to the NAS (network only: PSRAM stack).
        xTaskCreatePinnedToCoreWithCaps(upload_task, "cam-upload", 12 * 1024, nullptr, 2, &uploader, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        G.started = true;
        set_status("Starting\xE2\x80\xA6");
    }
}

extern "C" int camera_app_take_photo(uint32_t timeout_ms)
{
    if (!G.started || G.shutdown) {
        return -1;
    }
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    G.headless = true;
    while (!G.streaming && xTaskGetTickCount() < until) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    int index = -1;
    if (G.streaming) {
        while (G.capture && xTaskGetTickCount() < until) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        G.last_luma = -1;
        const uint32_t before = G.stored_count;
        G.capture = true;
        // Wait for the photo itself (JPEG encoded and stored), not just the frame.
        while (G.stored_count == before && xTaskGetTickCount() < until) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        if (G.stored_count != before) {
            std::lock_guard<std::mutex> guard(G.lock);
            index = int(G.photos.size()) - 1;
        }
    }
    G.headless = false;
    G.ui_dirty = true;
    return index;
}

extern "C" bool camera_app_torch(bool on)
{
    if (!G.started || G.shutdown) {
        return false;
    }
    G.torch_only = on;
    if (!on) {
        return true;
    }
    for (int i = 0; i < 100 && G.torch_only && !G.torch_lit; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return G.torch_lit;
}

extern "C" void camera_app_set_brightness_hook(void (*set)(int percent))
{
    brightness_hook = set;
}
extern "C" void camera_app_set_gallery_hook(void (*open)(void))
{
    gallery_hook = open;
}

extern "C" bool camera_app_photo_info(int index, uint32_t *at, bool *on_nas, uint32_t *size)
{
    std::lock_guard<std::mutex> guard(G.lock);
    if (index < 0 || index >= int(G.photos.size())) {
        return false;
    }
    const Photo &p = G.photos[size_t(index)];
    if (at) {
        *at = p.at;
    }
    if (on_nas) {
        *on_nas = !p.nas.empty();
    }
    if (size) {
        *size = uint32_t(p.size);
    }
    return true;
}

extern "C" bool camera_app_delete_photo(int index)
{
    std::string nas;
    {
        std::lock_guard<std::mutex> guard(G.lock);
        if (index < 0 || index >= int(G.photos.size())) {
            return false;
        }
        Photo &p = G.photos[size_t(index)];
        if (p.jpeg) {
            G.photo_bytes -= p.size;
            heap_caps_free(p.jpeg);
        }
        if (!p.path.empty()) {
            unlink(p.path.c_str());
        }
        nas = p.nas;
        G.photos.erase(G.photos.begin() + index);
        ++photos_revision;
    }
    if (!nas.empty()) {
        files_nas_delete(nas.c_str());
    }
    G.ui_dirty = true;
    return true;
}

extern "C" uint32_t camera_app_photos_revision(void)
{
    return photos_revision;
}

extern "C" void camera_app_refresh_photos(void)
{
    relist_wanted = true;
    if (uploader) {
        xTaskNotifyGive(uploader);
    }
}

extern "C" int camera_app_photos_state(void)
{
    return nas_list_state;
}

extern "C" void camera_app_pick(void (*done)(int index))
{
    G.pick = done;
    if (done) {
        set_status("Photo for Swifty");
    }
}

extern "C" int camera_app_photo_count(void)
{
    std::lock_guard<std::mutex> guard(G.lock);
    return int(G.photos.size());
}

extern "C" bool camera_app_photo_jpeg(int index, uint8_t **jpeg, size_t *size)
{
    {
        std::lock_guard<std::mutex> guard(G.lock);
        const int n = int(G.photos.size());
        if (index < 0) {
            index += n;
        }
        if (index < 0 || index >= n) {
            return false;
        }
    }
    std::vector<uint8_t> bytes;
    if (!photo_bytes(index, bytes)) {
        return false;
    }
    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(bytes.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) {
        return false;
    }
    memcpy(copy, bytes.data(), bytes.size());
    *jpeg = copy;
    *size = bytes.size();
    return true;
}

extern "C" void camera_app_set_active(bool active)
{
    if (!active && G.pick) {
        G.pick = nullptr;    // left without taking one
    }
    G.active = active && G.started;
    if (!G.active && G.gallery >= 0) {
        close_gallery_cb(nullptr);
    }
}

// A restart's shutdown handler. The restart resets the CPUs, not every DMA
// engine (ESP-Iris notes the same for USB), so a stream still running keeps
// writing frames into the next boot's memory: that boot then had no USB until
// Vibe Mode. So the stream must stop first, but without blocking forever:
// stopping it here waited on the camera's lock, which the capture task holds
// while it waits for a frame (the restart then ended by watchdog). The capture
// task stops the stream itself at its next frame; this waits for that.
extern "C" void camera_app_set_frame_sink(void (*sink)(const unsigned char *jpeg, size_t length),
                                          camera_webcam_view_t view)
{
    if (sink) {
        G.webcam_view = view;    // before the sink: the capture task reads both
    }
    G.webcam_sink = sink;
    ESP_LOGI(TAG, "webcam frames %s", sink ? "on" : "off");
}

extern "C" void camera_app_webcam_yuv(bool on)
{
    G.wc_yuv = on;
}

extern "C" void camera_app_webcam_stats(char *out, size_t cap, bool reset)
{
    const int n = std::max(WS.n, 1);
    snprintf(out, cap, "%s %d frames: PPA %.1f ms, JPEG %.1f ms, all %.1f ms (worst %.1f), last %lu bytes",
             G.wc_yuv ? "YUV420" : "RGB565", WS.n, WS.ppa / 1000.0 / n, WS.jpeg / 1000.0 / n, WS.total / 1000.0 / n,
             WS.worst / 1000.0, (unsigned long)WS.bytes);
    if (reset) WS = {};
}

extern "C" void camera_app_set_auto_facing(bool on)
{
    if (G.auto_facing == on) {
        return;
    }
    G.auto_facing = on;
    if (on) {
        swivel_quiet_until = esp_timer_get_time() + 1000000;    // not on the picture from before
    }
    save_prefs();
    ESP_LOGI(TAG, "front/rear detection %s", on ? "automatic" : "manual");
}

extern "C" bool camera_app_auto_facing(void)
{
    return G.auto_facing;
}

extern "C" void camera_app_shutdown(void)
{
    G.shutdown = true;
    G.active = false;
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return;
    }
    // 1.5 s at most, in whole ticks (100 Hz: 5 ms was 0 ticks, no wait at all).
    for (int i = 0; i < 150 && G.streaming; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void camera_app_summary(camera_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    out->started = G.started;
    out->present = G.present;
    out->streaming = G.streaming;
    {
        std::lock_guard<std::mutex> guard(G.lock);
        out->photos = int(G.photos.size());
    }
    strlcpy(out->status, G.status[0] ? G.status : (G.streaming ? "Live" : ""), sizeof(out->status));
}
