// SPDX-License-Identifier: Apache-2.0
// Face ID on the board. ESP-DL's face detector (MSR + MNP: a box and five
// landmarks) and MobileFaceNet (a 112 x 112 aligned crop to a unit vector) run
// on 240 x 240 frames borrowed from the camera app; a template per profile is
// the average of several embeddings, kept as int8 in NVS (namespace face_id).
// Nothing about a face (frames, embeddings, templates) leaves the board: the
// test RPC (54 · 62) reports only results, similarities and timings.
//
// Threads: the API, the borrow and every NVS write run on the LVGL task (a
// "pump" timer exists while an operation, a preview or the borrow does). The
// camera's capture task hands frames over in on_frame (a copy for the worker,
// the preview's conversion). The worker (core 1, where the S31's PIE vector
// unit is; priority 1, below the UI and the network; PSRAM stack: it never
// touches flash) runs the models and posts one outcome per operation, tagged
// with the operation's number so a cancelled one is ignored.
#include "face_id.h"
#include "face_id_core.h"
#include "face_match.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <new>

#include "bsp/esp_mosaico.h"
#include "camera_app.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "iris_ext.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "nvs.h"

namespace {
using face_core::Status;
const char *TAG = "face_id";

constexpr int FRAME = 240;  // the camera's 640 x 640 centre at 6/16: faces at arm's length span ~100 px
constexpr size_t FRAME_BYTES = size_t(FRAME) * FRAME * 3;
// Eye distance in frame pixels (~56 at 40 cm, ~28 at 80 cm): below these the
// 112 x 112 crop is mostly upscaled blur, and landmark jitter swamps the
// liveness check's movement.
constexpr float MIN_EYES_IDENTIFY = 28.0f, MIN_EYES_ENROLL = 32.0f;
constexpr int ENROLL_SAMPLES = 5;
constexpr float ENROLL_COSINE = 0.70f;  // each sample against their mean: one face, cleanly cropped
// Enrollment wants the face roughly frontal: the nose's place in the
// eyes-mouth frame (frontal a = 0.25, b = 0.5; see face_match.h).
constexpr float FRONTAL_A = 0.25f, FRONTAL_B = 0.5f, FRONTAL_A_TOL = 0.12f, FRONTAL_B_TOL = 0.2f;
constexpr uint32_t ENROLL_TIMEOUT_MS = 20000, IDENTIFY_DEFAULT_MS = 10000, IDENTIFY_MAX_MS = 60000;
constexpr int64_t NO_FRAMES_US = 4000000, BUSY_RETRY_US = 1000000, NO_CAMERA_RETRY_US = 5000000,
                  FINAL_STATUS_US = 4000000, IDLE_UNLOAD_US = 60000000;
constexpr size_t MODEL_HEADROOM = 1536 * 1024;  // PSRAM the two models' arenas need, with room to spare
constexpr uint8_t RPC_FACE = 62;  // the sub-command of RPC 54 (iris_ext.h)
constexpr const char *NVS_NAMESPACE = "face_id";
constexpr uint8_t TUNING_VERSION = 1;

enum class Op : uint8_t { None, Enroll, Identify };

struct Request {  // LVGL task -> worker
    uint32_t gen = 0;
    Op op = Op::None;
    uint8_t profile = 0;
    uint32_t timeout_ms = 0;
};

struct Outcome {  // worker -> LVGL task; gen 0: nothing posted
    uint32_t gen = 0;
    face_id_result_t result = FACE_ID_ERROR;
    int profile = -1;
    float similarity = 0, best = -1, spread = 0;
    int frames = 0;  // frames measured (detector run)
};

struct Last {  // the last finished operation (RPC 54 · 62)
    Op op = Op::None;
    face_id_result_t result = FACE_ID_ERROR;
    int profile = -1;
    float similarity = 0, best = -1, spread = 0;
    uint32_t ms = 0;
    int frames = 0;
};

struct TuningBlob {
    uint8_t version, agree, liveness, reserved;
    float threshold, margin, spread;
};

struct Core {
    // LVGL task
    void (*camera_ready)() = nullptr;
    lv_timer_t *pump = nullptr;
    uint32_t next_gen = 1;
    struct {
        bool active = false, has_pending = false;
        uint32_t gen = 0;
        Op kind = Op::None;
        uint8_t profile = 0;
        void (*enroll_done)(face_id_result_t, void *) = nullptr;
        void (*identify_done)(face_id_match_t, void *) = nullptr;
        void *user = nullptr;
        face_id_result_t pending = FACE_ID_ERROR;
        int64_t started_us = 0;
    } op;
    bool borrowed = false, preview_visible = false;
    int64_t borrow_retry_us = 0, borrowed_at_us = 0, final_until_us = 0;
    Last last;

    // Shared, under lock
    std::mutex lock;
    Request request;
    Outcome outcome;
    face::Tuning tuning;
    bool enrolled[face::PROFILES] = {};
    uint16_t lengths[face::PROFILES] = {};
    float templates[face::PROFILES][face::MAX_FEAT];
    float new_template[face::MAX_FEAT];  // an enrollment's result, until stored
    face::Template blob;  // NVS staging (LVGL task)

    // The frame handoff and the preview, under frame_lock
    std::mutex frame_lock;
    uint8_t *work = nullptr;  // the worker's frame (BGR888)
    face_core::PreviewFrames *preview = nullptr;

    std::atomic<bool> want_frame{false}, lost{false}, model_failed{false};
    std::atomic<uint8_t> status{uint8_t(Status::Idle)};
    std::atomic<uint32_t> frames{0};  // camera frames since the borrow
    std::atomic<bool> rear{false};
    std::atomic<uint32_t> detect_us{0}, embed_us{0};  // running averages
    std::atomic<int> feat_len{0};
    std::atomic<uint32_t> model_bytes{0};

    // Worker only
    TaskHandle_t worker = nullptr;
    HumanFaceDetect *detect = nullptr;
    HumanFaceFeat *feat = nullptr;
    face::Enrollment enrollment;
    float query[face::MAX_FEAT];
};
Core *C = nullptr;

Status current_status()
{
    return Status(C->status.load());
}

void set_status(Status s)
{
    C->status = uint8_t(s);
}

// From the worker: only while its operation is still the one asked for (a
// cancelled one must not overwrite the final status).
void worker_status(uint32_t gen, Status s)
{
    std::lock_guard<std::mutex> guard(C->lock);
    if (C->request.gen == gen) {
        C->status = uint8_t(s);
    }
}

const char *result_name(face_id_result_t r)
{
    static const char *const NAMES[] = {"ok", "no_camera", "no_face", "many_faces", "no_match",
                                        "busy", "cancelled", "timeout", "error"
                                       };
    return unsigned(r) < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[r] : "?";
}

// ---- NVS (LVGL task) -----------------------------------------------------------------------
// Plain NVS: the project enables neither flash nor NVS encryption, so a
// template is readable by anyone who can dump the flash (see README.md).
struct Key {
    char text[8];
};
Key template_key(int profile)  // "p0" ... "p15"
{
    Key k;
    snprintf(k.text, sizeof(k.text), "p%u", unsigned(profile) % face::PROFILES);
    return k;
}

void load_nvs()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;    // nothing saved yet
    }
    TuningBlob tb = {};
    size_t size = sizeof(tb);
    if (nvs_get_blob(h, "tune", &tb, &size) == ESP_OK && size == sizeof(tb) && tb.version == TUNING_VERSION) {
        face::Tuning t;
        t.threshold = tb.threshold, t.margin = tb.margin, t.agree = tb.agree;
        t.liveness = tb.liveness, t.live_spread = tb.spread;
        if (face::tuning_valid(t)) {
            C->tuning = t;
        }
    }
    int count = 0;
    for (int p = 0; p < face::PROFILES; ++p) {
        size = sizeof(C->blob);
        if (nvs_get_blob(h, template_key(p).text, &C->blob, &size) != ESP_OK) {
            continue;
        }
        const int n = C->blob.length;
        if (face::dequantize(C->blob, size, C->templates[p], n)) {
            C->enrolled[p] = true;
            C->lengths[p] = uint16_t(n);
            ++count;
        } else {
            ESP_LOGW(TAG, "profile %d: template from another model or version, ignored (enroll again)", p);
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d profile(s) enrolled", count);
}

bool save_tuning()
{
    face::Tuning t;
    {
        std::lock_guard<std::mutex> guard(C->lock);
        t = C->tuning;
    }
    TuningBlob tb = {TUNING_VERSION, uint8_t(t.agree), uint8_t(t.liveness), 0, t.threshold, t.margin, t.live_spread};
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_blob(h, "tune", &tb, sizeof(tb)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool store_template(int profile, const float *v, int n)
{
    const size_t bytes = face::quantize(v, n, C->blob);
    nvs_handle_t h;
    if (!bytes || nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_blob(h, template_key(profile).text, &C->blob, bytes) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) {
        return false;
    }
    // What matching uses is exactly what a restart will load.
    std::lock_guard<std::mutex> guard(C->lock);
    face::dequantize(C->blob, bytes, C->templates[profile], n);
    C->enrolled[profile] = true;
    C->lengths[profile] = uint16_t(n);
    return true;
}

void erase_template(int profile)
{
    {
        std::lock_guard<std::mutex> guard(C->lock);
        C->enrolled[profile] = false;
        C->lengths[profile] = 0;
        memset(C->templates[profile], 0, sizeof(C->templates[profile]));
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, template_key(profile).text);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- camera frames (the capture task) ----------------------------------------------------
void on_frame(const uint8_t *bgr, int size, bool rear, void *)
{
    ++C->frames;
    C->rear = rear;
    std::unique_lock<std::mutex> guard(C->frame_lock, std::try_to_lock);
    if (!guard.owns_lock() || size != FRAME) {
        return;    // a preview being detached: skip this frame
    }
    if (C->want_frame && C->work) {
        memcpy(C->work, bgr, FRAME_BYTES);
        C->want_frame = false;
        xTaskNotifyGive(C->worker);
    }
    if (C->preview) {
        C->preview->fill_from_bgr(bgr, size);
    }
}

void on_lost(void *)
{
    C->lost = true;  // the pump ends the operation (LVGL task)
}

// ---- the worker ------------------------------------------------------------------------------
bool load_models()
{
    if (C->detect && C->feat) {
        return true;
    }
    // The weights stay where the app image already is (PSRAM, XIP); this is
    // the models' working memory.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < MODEL_HEADROOM) {
        ESP_LOGE(TAG, "not enough PSRAM for the models (largest block %u)",
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(C->frame_lock);
        if (!C->work) {
            C->work = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
    }
    if (!C->work) {
        return false;
    }
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const int64_t t0 = esp_timer_get_time();
    C->detect = new (std::nothrow) HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
    C->feat = new (std::nothrow) HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false);
    if (!C->detect || !C->feat) {
        delete C->detect;
        delete C->feat;
        C->detect = nullptr;
        C->feat = nullptr;
        return false;
    }
    const int len = C->feat->get_feat_len();
    if (len <= 0 || len > face::MAX_FEAT) {
        ESP_LOGE(TAG, "unexpected embedding length %d", len);
        C->model_failed = true;  // a build problem, not a passing shortage: face ID stays off
        return false;
    }
    C->feat_len = len;
    C->model_bytes = uint32_t(before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "models ready in %d ms: %u bytes of PSRAM, %d-value embeddings",
             int((esp_timer_get_time() - t0) / 1000), unsigned(C->model_bytes.load()), len);
    return true;
}

void unload_models()
{
    delete C->detect;
    delete C->feat;
    C->detect = nullptr;
    C->feat = nullptr;
    std::lock_guard<std::mutex> guard(C->frame_lock);
    C->want_frame = false;
    heap_caps_free(C->work);
    C->work = nullptr;
    ESP_LOGI(TAG, "models unloaded while idle");
}

uint32_t average(std::atomic<uint32_t> &avg, int64_t sample)
{
    const uint32_t v = avg ? uint32_t((avg * 7 + uint32_t(sample)) / 8) : uint32_t(sample);
    avg = v;
    return v;
}

// One operation's progress, on the worker's stack (small: the big buffers are in Core).
struct Run {
    uint32_t gen = 0;
    Op op = Op::None;
    uint8_t profile = 0;
    int64_t deadline_us = 0;
    face::Tuning tuning;
    face::Agreement agreement;
    face::Liveness liveness;
    int frames = 0, measured = 0;
    bool saw_face = false, last_many = false;
    float best = -1;
};

void post(const Run &run, face_id_result_t result, int profile, float similarity)
{
    std::lock_guard<std::mutex> guard(C->lock);
    if (C->request.gen != run.gen) {
        return;    // cancelled meanwhile
    }
    Outcome &o = C->outcome;
    o.gen = run.gen;
    o.result = result;
    o.profile = profile;
    o.similarity = similarity;
    o.best = run.best;
    o.spread = run.liveness.spread();
    o.frames = run.frames;
}

// What a run that ran out of time found most.
face_id_result_t timed_out(const Run &run)
{
    if (run.op == Op::Identify && run.agreement.count() >= run.tuning.agree) {
        return FACE_ID_TIMEOUT;    // matched, never moved
    }
    if (run.measured) {
        return run.op == Op::Identify ? FACE_ID_NO_MATCH : FACE_ID_TIMEOUT;
    }
    return run.last_many ? FACE_ID_MANY_FACES : run.saw_face ? FACE_ID_TIMEOUT : FACE_ID_NO_FACE;
}

// One frame of an operation; true when it is finished (outcome posted).
bool process(Run &run)
{
    dl::image::img_t img = {};
    img.data = C->work;
    img.width = FRAME;
    img.height = FRAME;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_BGR888;  // the PPA's RGB888 byte order
    ++run.frames;
    int64_t t0 = esp_timer_get_time();
    std::list<dl::detect::result_t> &faces = C->detect->run(img);
    average(C->detect_us, esp_timer_get_time() - t0);
    if (faces.empty()) {
        run.agreement.reset();
        run.liveness.reset();
        run.last_many = false;
        worker_status(run.gen, C->rear ? Status::Rear : Status::Look);
        return false;
    }
    run.saw_face = true;
    run.last_many = faces.size() > 1;
    if (run.last_many) {
        run.agreement.reset();
        run.liveness.reset();
        worker_status(run.gen, Status::OneFace);
        return false;
    }
    const dl::detect::result_t &face = faces.front();
    if (face.keypoint.size() != 10) {
        return false;
    }
    const face::Landmarks marks = face::landmarks_from(face.keypoint.data());
    const bool inside = face.box[0] > 1 && face.box[1] > 1 && face.box[2] < FRAME - 2 && face.box[3] < FRAME - 2;
    const float eyes = face::eye_distance(marks);
    if (!inside) {
        worker_status(run.gen, Status::Look);
        return false;
    }
    if (eyes < (run.op == Op::Enroll ? MIN_EYES_ENROLL : MIN_EYES_IDENTIFY)) {
        worker_status(run.gen, Status::Closer);
        return false;
    }
    const face::NosePose pose = face::nose_pose(marks);
    if (run.op == Op::Enroll &&
            (!pose.valid || fabsf(pose.a - FRONTAL_A) > FRONTAL_A_TOL || fabsf(pose.b - FRONTAL_B) > FRONTAL_B_TOL)) {
        worker_status(run.gen, Status::Look);  // turned away: look straight at it
        return false;
    }
    worker_status(run.gen, Status::Hold);
    t0 = esp_timer_get_time();
    dl::TensorBase *embedding = C->feat->run(img, face.keypoint);
    average(C->embed_us, esp_timer_get_time() - t0);
    const int n = C->feat_len;
    if (!embedding || embedding->get_size() != n) {
        return false;
    }
    memcpy(C->query, embedding->data, sizeof(float) * size_t(n));
    ++run.measured;

    if (run.op == Op::Enroll) {
        C->enrollment.add(C->query);
        if (C->enrollment.count() >= ENROLL_SAMPLES) {
            bool done;
            {
                std::lock_guard<std::mutex> guard(C->lock);
                done = C->enrollment.finish(C->new_template, ENROLL_SAMPLES, ENROLL_COSINE);
                if (done) {
                    // Someone already enrolled under another profile? Allowed,
                    // but identify then needs the margin between the two.
                    const float *others[face::PROFILES] = {};
                    for (int p = 0; p < face::PROFILES; ++p) {
                        others[p] = C->enrolled[p] && C->lengths[p] == n && p != run.profile ? C->templates[p] : nullptr;
                    }
                    const face::Match m = face::best_match(C->new_template, n, others, C->tuning);
                    run.best = m.best;
                    if (m.best >= C->tuning.threshold) {
                        ESP_LOGW(TAG, "profile %d's face is close to profile %d's (%.2f)", run.profile, m.best_profile,
                                 double(m.best));
                    }
                }
            }
            if (done) {
                post(run, FACE_ID_OK, run.profile, 1.0f);
                return true;
            }
        }
        return false;
    }

    face::Match m;
    {
        std::lock_guard<std::mutex> guard(C->lock);
        const float *templates[face::PROFILES] = {};
        for (int p = 0; p < face::PROFILES; ++p) {
            templates[p] = C->enrolled[p] && C->lengths[p] == n ? C->templates[p] : nullptr;
        }
        m = face::best_match(C->query, n, templates, run.tuning);
    }
    run.best = fmaxf(run.best, m.best);
    const int before_profile = run.agreement.profile(), before_count = run.agreement.count();
    const bool agreed = run.agreement.push(m.profile, m.best, run.tuning.agree);
    if (run.agreement.profile() != before_profile || run.agreement.count() < before_count) {
        run.liveness.reset();  // a new run: its movement starts now
    }
    if (m.profile >= 0 && m.profile == run.agreement.profile()) {
        run.liveness.push(marks);
    }
    ESP_LOGD(TAG, "best %d %.3f second %.3f, run %d x%d, spread %.3f", m.best_profile, double(m.best),
             double(m.second), run.agreement.profile(), run.agreement.count(), double(run.liveness.spread()));
    if (agreed) {
        if (!run.tuning.liveness || run.liveness.passed(run.tuning.live_spread)) {
            post(run, FACE_ID_OK, run.agreement.profile(), run.agreement.mean_similarity());
            return true;
        }
        worker_status(run.gen, Status::Turn);
    }
    return false;
}

void worker_task(void *)
{
    Run run;
    bool waiting = false;
    int64_t idle_since = esp_timer_get_time();
    for (;;) {
        const bool loaded = C->detect != nullptr;
        const TickType_t wait = run.op != Op::None ? pdMS_TO_TICKS(100) : loaded ? pdMS_TO_TICKS(5000) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
        Request r;
        {
            std::lock_guard<std::mutex> guard(C->lock);
            r = C->request;
        }
        const int64_t now = esp_timer_get_time();
        if (r.gen != run.gen) {  // a new operation, or the last one cancelled
            run = Run();
            run.gen = r.gen;
            run.op = r.op;
            run.profile = r.profile;
            {
                std::lock_guard<std::mutex> guard(C->lock);
                run.tuning = C->tuning;
            }
            waiting = false;
            if (run.op != Op::None) {
                if (!load_models()) {
                    post(run, FACE_ID_ERROR, -1, 0);
                    run.op = Op::None;
                } else {
                    run.deadline_us = now + int64_t(r.timeout_ms) * 1000;
                    C->enrollment.reset(C->feat_len);
                    C->want_frame = true;
                    waiting = true;
                }
            }
            idle_since = now;
        }
        if (run.op == Op::None) {
            C->want_frame = false;
            if (loaded && now - idle_since > IDLE_UNLOAD_US) {
                unload_models();
            }
            continue;
        }
        if (now >= run.deadline_us) {
            post(run, timed_out(run), -1, 0);
            run.op = Op::None;
            idle_since = now;
            continue;
        }
        if (!waiting || C->want_frame) {
            continue;    // no frame yet
        }
        const bool finished = process(run);
        if (finished) {
            run.op = Op::None;
            idle_since = esp_timer_get_time();
            continue;
        }
        // Let the idle task and anything at priority 0-1 breathe between
        // inferences (each is ~180 ms of one core).
        vTaskDelay(pdMS_TO_TICKS(10));
        C->want_frame = true;
    }
}

// ---- the pump (LVGL task) -------------------------------------------------------------------
void pump_tick(lv_timer_t *);

void ensure_pump()
{
    if (!C->pump) {
        C->pump = lv_timer_create(pump_tick, 66, nullptr);
    }
}

Status final_status(Op kind, face_id_result_t r)
{
    switch (r) {
    case FACE_ID_OK: return kind == Op::Enroll ? Status::Saved : Status::Recognised;
    case FACE_ID_BUSY: return Status::Busy;
    case FACE_ID_NO_CAMERA: return Status::NoCamera;
    case FACE_ID_ERROR: return Status::Error;
    case FACE_ID_CANCELLED: return Status::Idle;
    default: return kind == Op::Identify ? Status::NotRecognised : Status::Look;
    }
}

// Ends the operation and calls its done (LVGL task). The worker drops it.
void finish(face_id_result_t result, int profile, float similarity, const Outcome *o)
{
    if (!C->op.active) {
        return;
    }
    const Op kind = C->op.kind;
    const uint8_t enroll_profile = C->op.profile;
    const int64_t started_us = C->op.started_us;
    auto *enroll_done = C->op.enroll_done;
    auto *identify_done = C->op.identify_done;
    void *user = C->op.user;
    C->op = {};
    {
        std::lock_guard<std::mutex> guard(C->lock);
        C->request = Request();
        C->request.gen = C->next_gen++;
        C->outcome.gen = 0;
    }
    xTaskNotifyGive(C->worker);
    if (kind == Op::Enroll && result == FACE_ID_OK) {
        float *v;
        {
            std::lock_guard<std::mutex> guard(C->lock);
            v = C->new_template;  // the worker writes it only during an enrollment, and none runs now
        }
        if (!store_template(enroll_profile, v, C->feat_len)) {
            ESP_LOGE(TAG, "profile %d: the template could not be saved", enroll_profile);
            result = FACE_ID_ERROR;
        }
    }
    const int64_t now = esp_timer_get_time();
    C->last.op = kind;
    C->last.result = result;
    C->last.profile = result == FACE_ID_OK ? (kind == Op::Enroll ? enroll_profile : profile) : -1;
    C->last.similarity = result == FACE_ID_OK ? similarity : 0;
    C->last.best = o ? o->best : -1;
    C->last.spread = o ? o->spread : 0;
    C->last.frames = o ? o->frames : 0;
    C->last.ms = uint32_t((now - started_us) / 1000);
    set_status(final_status(kind, result));
    C->final_until_us = now + FINAL_STATUS_US;
    ESP_LOGI(TAG, "%s: %s%s", kind == Op::Enroll ? "enroll" : "identify", result_name(result),
             result == FACE_ID_OK && kind == Op::Identify ? " (a profile matched)" : "");
    if (kind == Op::Enroll && enroll_done) {
        enroll_done(result, user);
    } else if (kind == Op::Identify && identify_done) {
        face_id_match_t m = {};
        m.result = result;
        m.profile = result == FACE_ID_OK ? uint8_t(profile) : 0xFF;
        m.similarity = result == FACE_ID_OK ? similarity : (o ? fmaxf(o->best, 0.0f) : 0.0f);
        identify_done(m, user);
    }
}

void try_borrow(int64_t now)
{
    camera_borrow_t b = {};
    b.size = FRAME;
    b.frame = on_frame;
    b.lost = on_lost;
    camera_borrow_result_t r = camera_app_borrow(&b);
    if (r == CAMERA_BORROW_NOT_STARTED && C->camera_ready) {
        C->camera_ready();  // builds the Camera's screen and its buffers once
        r = camera_app_borrow(&b);
    }
    if (r == CAMERA_BORROW_OK) {
        C->borrowed = true;
        C->borrowed_at_us = now;
        C->frames = 0;
        C->lost = false;
        if (now >= C->final_until_us || C->op.active) {
            set_status(Status::Starting);
        }
        return;
    }
    C->borrow_retry_us = now + BUSY_RETRY_US;
    const face_id_result_t why = r == CAMERA_BORROW_BUSY ? FACE_ID_BUSY : FACE_ID_ERROR;
    set_status(final_status(Op::None, why));
    if (C->op.active && !C->op.has_pending) {
        C->op.pending = why;
        C->op.has_pending = true;  // done waits for the next tick: never inside the start call
    }
}

void pump_tick(lv_timer_t *)
{
    const int64_t now = esp_timer_get_time();
    if (C->lost.exchange(false) && C->borrowed) {
        C->borrowed = false;
        C->borrow_retry_us = now + BUSY_RETRY_US;
        if (C->op.active) {
            finish(FACE_ID_BUSY, -1, 0, nullptr);
        }
        set_status(Status::Busy);
        C->final_until_us = now + FINAL_STATUS_US;
    }
    if (C->op.active && C->op.has_pending) {
        finish(C->op.pending, -1, 0, nullptr);
    }
    const bool want = C->op.active || (C->preview && C->preview_visible);
    if (want && !C->borrowed && now >= C->borrow_retry_us) {
        try_borrow(now);
    } else if (!want && C->borrowed) {
        camera_app_borrow_end();
        C->borrowed = false;
    }
    if (C->borrowed) {
        const bool silent = C->frames == 0 && now - C->borrowed_at_us > NO_FRAMES_US;
        if (silent && camera_app_present() == 0) {
            // Nothing came and the camera app found no module: let it rest
            // and look again later.
            camera_app_borrow_end();
            C->borrowed = false;
            C->borrow_retry_us = now + NO_CAMERA_RETRY_US;
            if (C->op.active) {
                finish(FACE_ID_NO_CAMERA, -1, 0, nullptr);
            }
            set_status(Status::NoCamera);
            C->final_until_us = now + FINAL_STATUS_US;
        } else if (!C->op.active && C->frames > 0 && now >= C->final_until_us) {
            set_status(Status::Idle);  // the preview alone
        }
    }
    if (C->op.active) {
        Outcome o;
        {
            std::lock_guard<std::mutex> guard(C->lock);
            o = C->outcome;
            if (o.gen == C->op.gen) {
                C->outcome.gen = 0;
            }
        }
        if (o.gen && o.gen == C->op.gen) {
            finish(o.result, o.profile, o.similarity, &o);
        } else if (C->borrowed && C->frames > 0 && current_status() == Status::Starting) {
            set_status(Status::Look);
        }
    }
    if (!C->op.active && !C->preview && !C->borrowed) {
        lv_timer_delete(C->pump);
        C->pump = nullptr;
    }
}

bool start(Op kind, uint8_t profile, uint32_t timeout_ms)
{
    const int64_t now = esp_timer_get_time();
    C->op.active = true;
    C->op.has_pending = false;
    C->op.kind = kind;
    C->op.profile = profile;
    C->op.started_us = now;
    C->op.gen = C->next_gen++;
    bool any = false;
    {
        std::lock_guard<std::mutex> guard(C->lock);
        C->request.gen = C->op.gen;
        C->request.op = kind;
        C->request.profile = profile;
        C->request.timeout_ms = timeout_ms;
        C->outcome.gen = 0;
        for (bool e : C->enrolled) {
            any = any || e;
        }
    }
    C->final_until_us = 0;
    set_status(C->borrowed && C->frames > 0 ? Status::Look : Status::Starting);
    ensure_pump();
    if (kind == Op::Identify && !any) {
        C->op.pending = FACE_ID_NO_MATCH;  // nobody to match
        C->op.has_pending = true;
    } else if (!C->borrowed) {
        try_borrow(now);
    }
    xTaskNotifyGive(C->worker);
    return true;
}

bool can_start()
{
    return C && !C->op.active && !C->model_failed;
}

// ---- RPC 54 · 62 (ESP-Iris task) -------------------------------------------------------------
// The payload after iris_ext's sub-command byte: 00 (or nothing) status; 01 p enroll; 02 [u32 ms] identify; 03 cancel;
// 04 p forget; 05 tuning (u16 threshold, u16 margin in thousandths, u8 agree,
// u8 liveness, u16 spread in thousandths). The reply is the status as JSON,
// before the change; only results, similarities and timings, never face data.
size_t status_json(char *out, size_t cap)
{
    face::Tuning t;
    char enrolled[64] = "";
    {
        std::lock_guard<std::mutex> guard(C->lock);
        t = C->tuning;
        size_t n = 0;
        for (int p = 0; p < face::PROFILES; ++p) {
            if (C->enrolled[p]) {
                n += size_t(snprintf(enrolled + n, sizeof(enrolled) - n, "%s%d", n ? "," : "", p));
            }
        }
    }
    const Last &l = C->last;
    static const char *const OPS[] = {"idle", "enroll", "identify"};
    const int len = snprintf(
                        out, cap,
                        "{\"available\":%s,\"camera\":%d,\"borrowed\":%s,\"frames\":%" PRIu32 ",\"model\":%s,\"embedding\":%d,"
                        "\"model_kb\":%" PRIu32 ",\"op\":\"%s\",\"status\":\"%s\",\"enrolled\":[%s],"
                        "\"tuning\":{\"threshold\":%.3f,\"margin\":%.3f,\"agree\":%d,\"liveness\":%s,\"spread\":%.3f},"
                        "\"ms\":{\"detect\":%.1f,\"embed\":%.1f},"
                        "\"last\":{\"op\":\"%s\",\"result\":\"%s\",\"profile\":%d,\"similarity\":%.3f,\"best\":%.3f,"
                        "\"spread\":%.3f,\"frames\":%d,\"ms\":%" PRIu32 "}}",
                        face_id_available() ? "true" : "false", camera_app_present(), C->borrowed ? "true" : "false",
                        C->frames.load(), C->detect ? "true" : "false", C->feat_len.load(), C->model_bytes.load() / 1024,
                        OPS[int(C->op.active ? C->op.kind : Op::None)], face_core::status_text(current_status()), enrolled,
                        double(t.threshold), double(t.margin), t.agree, t.liveness ? "true" : "false", double(t.live_spread),
                        C->detect_us.load() / 1000.0, C->embed_us.load() / 1000.0, OPS[int(l.op)],
                        l.op == Op::None ? "none" : result_name(l.result), l.profile, double(l.similarity), double(l.best),
                        double(l.spread), l.frames, l.ms);
    return len < 0 ? 0 : size_t(len) < cap ? size_t(len) : cap - 1;
}

struct RpcCall {
    uint8_t op;
    uint8_t data[8];
    size_t size;
};

void rpc_identify_done(face_id_match_t, void *) {}
void rpc_enroll_done(face_id_result_t, void *) {}

void rpc_apply(void *arg)
{
    RpcCall *c = static_cast<RpcCall *>(arg);
    uint32_t ms = 0;
    switch (c->op) {
    case 1:
        face_id_enroll_start(c->data[0], rpc_enroll_done, nullptr);
        break;
    case 2:
        if (c->size >= 4) {
            memcpy(&ms, c->data, 4);
        }
        face_id_identify_start(ms, rpc_identify_done, nullptr);
        break;
    case 3:
        face_id_cancel();
        break;
    case 4:
        face_id_forget(c->data[0]);
        break;
    case 5: {
        uint16_t thr, margin, spread;
        memcpy(&thr, c->data, 2);
        memcpy(&margin, c->data + 2, 2);
        memcpy(&spread, c->data + 6, 2);
        face::Tuning t;
        t.threshold = thr / 1000.0f;
        t.margin = margin / 1000.0f;
        t.agree = c->data[4];
        t.liveness = c->data[5] != 0;
        t.live_spread = spread / 1000.0f;
        if (face::tuning_valid(t)) {
            {
                std::lock_guard<std::mutex> guard(C->lock);
                C->tuning = t;
            }
            save_tuning();
        }
        break;
    }
    default:
        break;
    }
    delete c;
}

esp_err_t rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t cap, size_t *size, void *)
{
    *size = 0;
    const uint8_t op = request->payload_size ? request->payload[0] : 0;
    const size_t args = request->payload_size ? request->payload_size - 1 : 0;
    const bool needs_profile = op == 1 || op == 4;
    if (op > 5 || (needs_profile && (args < 1 || request->payload[1] >= face::PROFILES)) || (op == 5 && args < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    *size = status_json(reinterpret_cast<char *>(out), cap);
    if (op) {
        auto *c = new (std::nothrow) RpcCall{op, {}, args < 8 ? args : 8};
        if (c) {
            memcpy(c->data, request->payload + 1, c->size);
            lv_async_call(rpc_apply, c);
        }
    }
    bsp_display_unlock();
    return ESP_OK;
}
}  // namespace

// ---- the preview's side (face_id_core.h), LVGL task ----------------------------------------
namespace face_core {
void attach_preview(PreviewFrames *frames)
{
    if (!C) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(C->frame_lock);
        C->preview = frames;
    }
    C->preview_visible = true;
    ensure_pump();
}

void detach_preview(PreviewFrames *frames)
{
    if (!C) {
        return;
    }
    std::lock_guard<std::mutex> guard(C->frame_lock);  // waits for a frame being written into it
    if (C->preview == frames) {
        C->preview = nullptr;
        C->preview_visible = false;
    }
}

void preview_visible(bool visible)
{
    if (C) {
        C->preview_visible = visible;
    }
}

Status status()
{
    return C ? current_status() : Status::Error;
}
}  // namespace face_core

// ---- the API (LVGL task, or with its lock held) ----------------------------------------------
extern "C" void face_id_init(void (*camera_ready)(void))
{
    if (C) {
        return;
    }
    C = new (std::nothrow) Core;  // ~60 KB: PSRAM (malloc prefers it here)
    if (!C) {
        ESP_LOGE(TAG, "no memory");
        return;
    }
    C->camera_ready = camera_ready;
    load_nvs();
    if (xTaskCreatePinnedToCoreWithCaps(worker_task, "face_id", 16 * 1024, nullptr, 1, &C->worker, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "worker: no memory");
        C->model_failed = true;
        return;
    }
    // A sub-command of RPC 54: ESP-Iris's own table (32 handlers) is full.
    const esp_err_t err = iris_ext_register(RPC_FACE, rpc, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RPC 54 · %u not registered: %s", unsigned(RPC_FACE), esp_err_to_name(err));
    }
}

extern "C" bool face_id_available(void)
{
    // Before the camera has ever been opened its presence is unknown: say yes,
    // and an operation ends with FACE_ID_NO_CAMERA if there is none.
    return C && C->worker && !C->model_failed && camera_app_present() != 0;
}

extern "C" bool face_id_enroll_start(uint8_t profile, void (*done)(face_id_result_t, void *user), void *user)
{
    if (!can_start() || profile >= face::PROFILES || !done) {
        return false;
    }
    C->op.enroll_done = done;
    C->op.identify_done = nullptr;
    C->op.user = user;
    return start(Op::Enroll, profile, ENROLL_TIMEOUT_MS);
}

extern "C" bool face_id_identify_start(uint32_t timeout_ms, void (*done)(face_id_match_t, void *user), void *user)
{
    if (!can_start() || !done) {
        return false;
    }
    if (!timeout_ms) {
        timeout_ms = IDENTIFY_DEFAULT_MS;
    }
    timeout_ms = timeout_ms < 1000 ? 1000 : timeout_ms > IDENTIFY_MAX_MS ? IDENTIFY_MAX_MS : timeout_ms;
    C->op.enroll_done = nullptr;
    C->op.identify_done = done;
    C->op.user = user;
    return start(Op::Identify, 0, timeout_ms);
}

extern "C" void face_id_cancel(void)
{
    if (C && C->op.active) {
        finish(FACE_ID_CANCELLED, -1, 0, nullptr);
    }
}

extern "C" bool face_id_enrolled(uint8_t profile)
{
    if (!C || profile >= face::PROFILES) {
        return false;
    }
    std::lock_guard<std::mutex> guard(C->lock);
    return C->enrolled[profile];
}

extern "C" void face_id_forget(uint8_t profile)
{
    if (C && profile < face::PROFILES) {
        erase_template(profile);
        ESP_LOGI(TAG, "profile %u forgotten", unsigned(profile));
    }
}

extern "C" void face_id_set_liveness(bool on)
{
    if (!C) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(C->lock);
        if (C->tuning.liveness == on) {
            return;
        }
        C->tuning.liveness = on;
    }
    save_tuning();
}
