// SPDX-License-Identifier: Apache-2.0
// USB webcam mode: ESP-Iris stops (it owns TinyUSB through esp_tinyusb), then
// esp_tinyusb is installed again with a UVC camera's descriptors: one MJPEG
// format, 640 x 480 at 25 fps, on an isochronous endpoint (alternate setting 1;
// macOS never configured the camera with a bulk one). A task sends the latest
// pushed frame whenever the computer has the stream open. Stopping uninstalls
// it and starts ESP-Iris again.
// The descriptors follow Espressif's usb_device_uvc (Apache-2.0), which itself
// cannot be used here: it adds its own TinyUSB configuration and descriptor
// callbacks, which clash with ESP-Iris's esp_tinyusb.
#include "usb_webcam.h"

#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

namespace {
constexpr int RING = 10;  // control requests kept for the report
const char *TAG = "webcam";
constexpr uint8_t EP_IN = 0x81;
constexpr uint8_t STR_CAMERA = 4;
constexpr size_t MAX_FRAME = 512 * 1024;  // a 720p JPEG with room to spare

#define WEBCAM_CLOCK_HZ 27000000
#define WEBCAM_INPUT_TERMINAL 0x01
#define WEBCAM_OUTPUT_TERMINAL 0x02

// A UVC camera: video control (camera terminal -> streaming output terminal)
// and one video streaming interface with an MJPEG format and frame.
// The frame sizes, by UVC frame index (1-based): 720p first, the computer's
// usual pick; 640 x 480 for what asks for that.
constexpr int FRAME_W[] = {1280, 640}, FRAME_H[] = {720, 480};
constexpr int FRAMES = sizeof(FRAME_W) / sizeof(*FRAME_W);

#define WEBCAM_FRAME(_idx, _w, _h)                                                                                       \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT(_idx, 0, _w, _h, (_w) * (_h) * 16, (_w) * (_h) * 16 * USB_WEBCAM_FPS,           \
                                        (_w) * (_h) * 16 / 8, 10000000 / USB_WEBCAM_FPS, 10000000 / USB_WEBCAM_FPS,      \
                                        10000000, 10000000 / USB_WEBCAM_FPS)

#define WEBCAM_DESC_LEN                                                                                                      \
    (TUD_VIDEO_DESC_IAD_LEN + TUD_VIDEO_DESC_STD_VC_LEN + (TUD_VIDEO_DESC_CS_VC_LEN + 1) + TUD_VIDEO_DESC_CAMERA_TERM_LEN + \
     TUD_VIDEO_DESC_OUTPUT_TERM_LEN + TUD_VIDEO_DESC_STD_VS_LEN + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1) +                     \
     TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + 2 * TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN +                                    \
     TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN + TUD_VIDEO_DESC_STD_VS_LEN + 7)

#define WEBCAM_DESC(_epsize)                                                                                                   \
    TUD_VIDEO_DESC_IAD(0, 0x02, STR_CAMERA), TUD_VIDEO_DESC_STD_VC(0, 0, STR_CAMERA),                                        \
        TUD_VIDEO_DESC_CS_VC(0x0150, TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, WEBCAM_CLOCK_HZ, 1),    \
        TUD_VIDEO_DESC_CAMERA_TERM(WEBCAM_INPUT_TERMINAL, 0, 0, 0, 0, 0, 0),                                                 \
        TUD_VIDEO_DESC_OUTPUT_TERM(WEBCAM_OUTPUT_TERMINAL, VIDEO_TT_STREAMING, 0, 1, 0),                                     \
        TUD_VIDEO_DESC_STD_VS(1, 0, 0, STR_CAMERA),                                                                          \
        TUD_VIDEO_DESC_CS_VS_INPUT(1,                                                                                        \
                                   TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + 2 * TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN +         \
                                       TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN,                                              \
                                   EP_IN, 0, WEBCAM_OUTPUT_TERMINAL, 0, 0, 0, 0),                                            \
        TUD_VIDEO_DESC_CS_VS_FMT_MJPEG(1, 2, 0, 1, 0, 0, 0, 0), WEBCAM_FRAME(1, 1280, 720), WEBCAM_FRAME(2, 640, 480),       \
        TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709,                          \
                                            VIDEO_COLOR_COEF_SMPTE170M),                                                     \
        TUD_VIDEO_DESC_STD_VS(1, 1, 1, STR_CAMERA), TUD_VIDEO_DESC_EP_ISO(EP_IN, _epsize, 1)

constexpr size_t CONFIG_LEN = TUD_CONFIG_DESC_LEN + WEBCAM_DESC_LEN;

const tusb_desc_device_t DEVICE = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // An Interface Association Descriptor groups the video interfaces.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    // Espressif's vendor ID with a product ID of our choosing, distinct from
    // ESP-Iris's so the host sees a new device. Not allocated to this project:
    // before shipping devices, request one (https://github.com/espressif/usb-pids).
    .idVendor = 0x303A,
    .idProduct = 0x8102,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t HS_CONFIG[] = {TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_LEN, 0x80, 100), WEBCAM_DESC(1024)};
const uint8_t FS_CONFIG[] = {TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_LEN, 0x80, 100), WEBCAM_DESC(1023)};
static_assert(sizeof(HS_CONFIG) == CONFIG_LEN, "webcam configuration descriptor length");

char serial[13];
const char LANGID[] = {0x09, 0x04};
const char *STRINGS[] = {LANGID, "Glimdock", "ESP-Mosaico Camera", serial, "ESP-Mosaico Camera"};

std::atomic<bool> active{false}, stopping{false}, owns_usb{false};
std::mutex frame_lock;
uint8_t *latest = nullptr, *tx = nullptr;
size_t latest_len = 0;
uint32_t latest_seq = 0;
std::atomic<int> fps{0};
TaskHandle_t streamer = nullptr;
std::mutex switch_lock;
// The last session, for usb_webcam_report.
struct {
    int64_t started = 0, mounted_at = -1, streaming_at = -1;
    std::atomic<int> commits{0}, sent{0}, pushed{0}, mounts{0}, unmounts{0}, xfer_fail{0};
    uint32_t last_interval = 0, last_frame_index = 0;
    int opens = 0, open_result = -1, open_max_len = 0, edpt_test = -1;
    uint32_t open_class = 0;
    // The host's control requests to the camera, the latest RING of them:
    // ok -1 while the driver is still in it (a hang shows as -1).
    struct Req {
        uint32_t ms;
        uint8_t type, request, stage;
        int8_t ok;
        uint16_t value, index, length;
    };

    Req ring[RING] = {};
    std::atomic<int> requests{0}, xfers{0}, suspends{0}, resumes{0};
    int64_t last_suspend = -1, last_resume = -1;
    // Is TinyUSB's task alive: a call queued to it every 100 ms, how many ran
    // and the longest wait.
    std::atomic<int> pings{0}, pongs{0};
    std::atomic<int64_t> ping_sent{0}, ping_worst{0};
} R;

int64_t session_ms()
{
    return (esp_timer_get_time() - R.started) / 1000;
}

void pong(void *)
{
    const int64_t waited = esp_timer_get_time() - R.ping_sent;
    if (waited > R.ping_worst) {
        R.ping_worst = waited;
    }
    ++R.pongs;
}

bool video_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *rq)
{
    const int n = R.requests++;
    auto &e = R.ring[n % RING];
    e = {uint32_t(session_ms()), rq->bmRequestType, rq->bRequest, stage, -1, rq->wValue, rq->wIndex, rq->wLength};
    // A class request to a terminal (GET_INFO, GET_CUR, ... of a camera
    // control): TinyUSB's video driver answers "handled" without a reply, so
    // the transfer never ends, endpoint 0 never takes the next setup packet and
    // every request after it times out (macOS asks these before streaming).
    // The terminals have no controls: stall, the UVC answer for "not here".
    if (rq->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS && rq->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE &&
            tu_u16_low(rq->wIndex) == 0 && tu_u16_high(rq->wIndex) != 0) {
        e.ok = 0;
        return false;
    }
    const bool ok = videod_control_xfer_cb(rhport, stage, rq);
    e.ok = ok;
    return ok;
}

bool video_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t bytes)
{
    ++R.xfers;
    return videod_xfer_cb(rhport, ep, result, bytes);
}

// TinyUSB's video driver, wrapped while the webcam is installed, to see what
// its open() made of our interfaces (a refused open stalls the host's
// SET_CONFIGURATION, and the camera never shows up).
uint16_t video_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    ++R.opens;
    R.open_class = uint32_t(itf->bInterfaceClass) << 16 | uint32_t(itf->bInterfaceSubClass) << 8 | itf->bInterfaceProtocol;
    R.open_max_len = max_len;
    const uint16_t n = videod_open(rhport, itf, max_len);
    R.open_result = n;
    if (!n) {  // would the streaming endpoint open by itself?
        alignas(4) static const uint8_t ep[7] = {7, TUSB_DESC_ENDPOINT, EP_IN, TUSB_XFER_BULK, 0x00, 0x02, 1};  // bulk IN, 512
        R.edpt_test = usbd_edpt_open(rhport, reinterpret_cast<const tusb_desc_endpoint_t *>(ep)) ? 1 : 0;
        if (R.edpt_test) {
            usbd_edpt_close(rhport, EP_IN);
        }
    }
    return n;
}

const usbd_class_driver_t VIDEO_DRIVER = {
    "webcam", videod_init, videod_deinit, videod_reset, video_open, video_control, video_xfer, nullptr, nullptr,
};
std::atomic<bool> installing{false};

void usb_event(tinyusb_event_t *event, void *)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ++R.mounts;
        if (R.mounted_at < 0) {
            R.mounted_at = (esp_timer_get_time() - R.started) / 1000;
        }
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ++R.unmounts;
    } else if (event->id == TINYUSB_EVENT_SUSPENDED) {
        ++R.suspends;
        R.last_suspend = session_ms();
    } else if (event->id == TINYUSB_EVENT_RESUMED) {
        ++R.resumes;
        R.last_resume = session_ms();
    }
}

// Notification bits for the stream task: a frame went out, a new one came.
constexpr uint32_t SENT_BIT = 1, FRAME_BIT = 2;

// Sleeps until a frame is pushed or the last one went out (never polls: at
// the 100 Hz tick a short delay is 0 ticks, and the spin starved the screen).
void stream_task(void *)
{
    uint32_t sent_seq = 0;
    bool busy = false;
    int count = 0;
    int64_t window = esp_timer_get_time(), last_ping = 0;
    while (!stopping) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(50));
        if (bits & SENT_BIT) {
            ++count;
            busy = false;
        }
        const int64_t now = esp_timer_get_time();
        if (now - last_ping >= 100000 && R.pongs == R.pings) {  // one in flight at a time
            last_ping = now;
            R.ping_sent = now;
            ++R.pings;
            usbd_defer_func(pong, nullptr, false);
        }
        if (now - window >= 1000000) {
            fps = count;
            count = 0;
            window = now;
        }
        if (tud_mounted() && R.mounted_at < 0) {
            R.mounted_at = (esp_timer_get_time() - R.started) / 1000;
        }
        if (!tud_video_n_streaming(0, 0)) {
            busy = false;  // the computer closed the stream
            continue;
        }
        if (busy) {
            continue;    // tx is TinyUSB's until its last packet went out
        }
        size_t len = 0;
        {
            std::lock_guard<std::mutex> g(frame_lock);
            if (latest_seq != sent_seq && latest_len) {
                memcpy(tx, latest, latest_len);
                len = latest_len;
                sent_seq = latest_seq;
            }
        }
        if (!len) {
            continue;
        }
        if (R.streaming_at < 0) {
            R.streaming_at = (esp_timer_get_time() - R.started) / 1000;
        }
        busy = tud_video_n_frame_xfer(0, 0, tx, len);
        if (busy) {
            ++R.sent;
        } else {
            ++R.xfer_fail;
        }
    }
    streamer = nullptr;
    vTaskDelay(2);  // a push or a callback that read the handle just before may still notify it
    vTaskDeleteWithCaps(nullptr);  // its stack is in PSRAM
}
}  // namespace

extern "C" usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count)
{
    *count = installing ? 1 : 0;  // only for the webcam's TinyUSB, not ESP-Iris's
    return installing ? &VIDEO_DRIVER : nullptr;
}

extern "C" void tud_video_frame_xfer_complete_cb(uint_fast8_t, uint_fast8_t)
{
    if (streamer) {
        xTaskNotify(streamer, SENT_BIT, eSetBits);
    }
}

extern "C" int tud_video_commit_cb(uint_fast8_t, uint_fast8_t, video_probe_and_commit_control_t const *p)
{
    ESP_LOGI(TAG, "stream committed: frame %u, interval %lu x 100 ns", unsigned(p->bFrameIndex), (unsigned long)p->dwFrameInterval);
    ++R.commits;
    R.last_interval = p->dwFrameInterval;
    R.last_frame_index = p->bFrameIndex;
    return VIDEO_ERROR_NONE;
}

extern "C" esp_err_t usb_webcam_start(void)
{
    std::lock_guard<std::mutex> g(switch_lock);
    if (active) {
        return ESP_OK;
    }
    if (!latest) {
        latest = static_cast<uint8_t *>(heap_caps_malloc(MAX_FRAME, MALLOC_CAP_SPIRAM));
    }
    if (!tx) {
        tx = static_cast<uint8_t *>(heap_caps_malloc(MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    }
    if (!tx) {
        tx = static_cast<uint8_t *>(heap_caps_malloc(MAX_FRAME, MALLOC_CAP_SPIRAM));
    }
    if (!latest || !tx) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(serial, sizeof(serial), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGW(TAG, "USB webcam: ESP-Iris stops until the webcam does");
    owns_usb = true;
    esp_err_t err = esp_iris_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Iris did not stop: %s", esp_err_to_name(err));
        owns_usb = false;
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(300));  // the host sees the port go before the camera comes
    R.started = esp_timer_get_time();
    R.mounted_at = R.streaming_at = -1;
    R.commits = R.sent = R.pushed = R.mounts = R.unmounts = R.xfer_fail = 0;
    R.opens = 0, R.open_result = -1, R.open_max_len = 0, R.edpt_test = -1, R.open_class = 0;
    R.requests = R.xfers = R.suspends = R.resumes = R.pings = R.pongs = 0;
    R.last_suspend = R.last_resume = -1;
    R.ping_worst = 0;
    R.last_frame_index = R.last_interval = 0;
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event, nullptr);
    cfg.descriptor.device = &DEVICE;
    cfg.descriptor.string = STRINGS;
    cfg.descriptor.string_count = sizeof(STRINGS) / sizeof(*STRINGS);
    cfg.descriptor.full_speed_config = FS_CONFIG;
#if TUD_OPT_HIGH_SPEED
    cfg.descriptor.high_speed_config = HS_CONFIG;
#endif
    installing = true;
    err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "webcam USB: %s; ESP-Iris back", esp_err_to_name(err));
        installing = false;
        esp_iris_start();
        owns_usb = false;
        return err;
    }
    {
        std::lock_guard<std::mutex> f(frame_lock);
        latest_len = 0;
    }
    stopping = false;
    active = true;
    xTaskCreatePinnedToCoreWithCaps(stream_task, "webcam", 4096, nullptr, 4, &streamer, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "USB webcam on: MJPEG %dx%d or %dx%d", FRAME_W[0], FRAME_H[0], FRAME_W[1], FRAME_H[1]);
    return ESP_OK;
}

extern "C" esp_err_t usb_webcam_stop(void)
{
    std::lock_guard<std::mutex> g(switch_lock);
    if (!active) {
        return ESP_OK;
    }
    stopping = true;
    for (int i = 0; i < 100 && streamer; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    active = false;
    tinyusb_driver_uninstall();
    installing = false;
    vTaskDelay(pdMS_TO_TICKS(300));
    const esp_err_t err = esp_iris_start();
    owns_usb = false;
    ESP_LOGI(TAG, "USB webcam off; ESP-Iris %s", err == ESP_OK ? "back" : esp_err_to_name(err));
    fps = 0;
    return err;
}

extern "C" bool usb_webcam_active(void)
{
    return active;
}

extern "C" bool usb_webcam_owns_usb(void)
{
    return owns_usb;
}

extern "C" void usb_webcam_format(int *width, int *height)
{
    const uint32_t i = R.last_frame_index;
    const int f = i >= 1 && i <= uint32_t(FRAMES) ? int(i) - 1 : 0;
    *width = FRAME_W[f];
    *height = FRAME_H[f];
}

extern "C" bool usb_webcam_streaming(void)
{
    return active && tud_video_n_streaming(0, 0);
}

extern "C" void usb_webcam_push(const uint8_t *jpeg, size_t length)
{
    if (!active || !jpeg || !length || length > MAX_FRAME) {
        return;
    }
    {
        std::lock_guard<std::mutex> g(frame_lock);
        memcpy(latest, jpeg, length);
        latest_len = length;
        ++latest_seq;
        ++R.pushed;
    }
    if (TaskHandle_t t = streamer) {
        xTaskNotify(t, FRAME_BIT, eSetBits);
    }
}

extern "C" size_t usb_webcam_last_frame(size_t offset, uint8_t *out, size_t capacity)
{
    std::lock_guard<std::mutex> g(frame_lock);
    if (!latest || offset >= latest_len) {
        return 0;
    }
    const size_t n = std::min(capacity, latest_len - offset);
    memcpy(out, latest + offset, n);
    return n;
}

extern "C" int usb_webcam_fps(void)
{
    return active ? fps.load() : 0;
}

extern "C" void usb_webcam_report(char *out, size_t cap)
{
    // Read while the session runs or after it: "now" is when the report is made.
    const int64_t stuck = R.pings != R.pongs ? (esp_timer_get_time() - R.ping_sent) / 1000 : 0;
    int n = snprintf(out, cap,
                     "configured at %lld ms (attached %d, detached %d); streaming at %lld ms; commits %d (frame %lu, "
                     "interval %lu); frames pushed %d, sent %d, refused %d; open x%d -> %d; suspends %d (last %lld ms), "
                     "resumes %d (last %lld ms); TinyUSB task pings %d/%d, worst %lld ms%s%lld; xfers %d; %d requests:",
                     (long long)R.mounted_at, R.mounts.load(), R.unmounts.load(), (long long)R.streaming_at,
                     R.commits.load(), (unsigned long)R.last_frame_index, (unsigned long)R.last_interval,
                     R.pushed.load(), R.sent.load(), R.xfer_fail.load(), R.opens, R.open_result, R.suspends.load(),
                     (long long)R.last_suspend, R.resumes.load(), (long long)R.last_resume, R.pongs.load(),
                     R.pings.load(), (long long)(R.ping_worst / 1000), stuck ? ", STUCK for ms " : "",
                     (long long)(stuck ? stuck : 0), R.xfers.load(), R.requests.load());
    const int total = R.requests;
    for (int i = std::max(0, total - RING); i < total && n > 0 && size_t(n) < cap; ++i) {
        const auto &e = R.ring[i % RING];
        n += snprintf(out + n, cap - n, " %lums %02x/%02x v%04x i%04x l%u s%u%s", (unsigned long)e.ms, e.type,
                      e.request, e.value, e.index, e.length, e.stage, e.ok < 0 ? " HUNG" : e.ok ? "" : " stall");
    }
}
