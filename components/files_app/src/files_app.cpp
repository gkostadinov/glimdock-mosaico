// SPDX-License-Identifier: Apache-2.0
// Files: sources (the board, its photos, SMB shares) -> folders -> a preview.
// A worker task does all storage and network I/O; the UI posts it jobs and
// picks up results on an LVGL timer.
#include "files_app.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ArduinoJson.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mosaico_storage.h"
#include "mui.h"
#include "nvs.h"
#include "png.h"
#include "smb2/smb2.h"  // before libsmb2.h, which uses its types
#include "smb2/libsmb2.h"

namespace {
using namespace mui;
const char *TAG = "files";
// The Files colour: a fresh teal.
const Accent TEAL = {0x8CF5DC, 0x139E86, 0x3DDCBB, 0x022019, 0x0C2E28, 0x061613};
const Accent &ACC = TEAL;
constexpr size_t MAX_IMAGE_FILE = 12 * 1024 * 1024, MAX_TEXT = 32 * 1024;
constexpr int VIEW = 460;  // previews fit in this square
constexpr const char *NS = "files";

// ---- sources ---------------------------------------------------------------------------
struct Share {
    std::string name, host, share, user, password, path;
};
enum class Kind { Local, Smb };
struct Source {
    Kind kind;
    std::string name, detail, root;  // Local: absolute root; Smb: start folder in the share
    int share = -1;                  // index into shares
    bool writable = false;
};

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    uint32_t mtime = 0;
};

std::mutex cfg_lock;
std::vector<Share> shares;
// Where the camera keeps its photos: a share (by name) and a folder in it.
struct PhotoDest {
    std::string share, path;
} photo_dest;
constexpr const char *DEFAULT_PHOTO_PATH = "Mosaico Photos";
// One SMB connection, used by the Files worker and the camera's uploads.
std::recursive_mutex smb_lock;

void load_shares()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = 0;
    std::string text;
    if (nvs_get_str(h, "shares", nullptr, &n) == ESP_OK && n > 1) {
        text.resize(n);
        if (nvs_get_str(h, "shares", text.data(), &n) != ESP_OK) {
            text.clear();
        }
    }
    nvs_close(h);
    JsonDocument doc;
    if (text.empty() || deserializeJson(doc, text.c_str())) {
        return;
    }
    std::vector<Share> list;
    // Stored as {"shares": [...], "photos": {...}} (or, before, the bare list).
    JsonArrayConst arr = doc.is<JsonArray>() ? doc.as<JsonArrayConst>() : doc["shares"].as<JsonArrayConst>();
    for (JsonObjectConst s : arr)
        list.push_back({s["name"] | "", s["host"] | "", s["share"] | "", s["user"] | "", s["password"] | "", s["path"] | ""});
    PhotoDest dest{doc["photos"]["share"] | "", doc["photos"]["path"] | ""};
    std::fill(text.begin(), text.end(), '\0');
    std::lock_guard<std::mutex> g(cfg_lock);
    shares = std::move(list);
    photo_dest = dest;
}

bool jpeg_name(const std::string &n)
{
    auto ends = [&](const char *e) {
        const size_t l = strlen(e);
        return n.size() > l && strcasecmp(n.c_str() + n.size() - l, e) == 0;
    };
    return ends(".jpg") || ends(".jpeg");
}

bool image_name(const std::string &n)
{
    auto ends = [&](const char *e) {
        const size_t l = strlen(e);
        return n.size() > l && strcasecmp(n.c_str() + n.size() - l, e) == 0;
    };
    return ends(".jpg") || ends(".jpeg") || ends(".png");
}

bool text_name(const std::string &n)
{
    static const char *const EXT[] = {".txt", ".md", ".json", ".log", ".yaml", ".yml", ".csv", ".ini", ".conf", ".cfg",
                                      ".py", ".sh", ".c", ".h", ".cpp", ".js", ".ts", ".toml", ".xml", ".html", ".css",
                                      ".lua", ".rs", ".go", ".env.example", ".service"
                                     };
    for (const char *e : EXT) {
        const size_t l = strlen(e);
        if (n.size() > l && strcasecmp(n.c_str() + n.size() - l, e) == 0) {
            return true;
        }
    }
    return n == "README" || n == "LICENSE" || n == "Makefile" || n == "Dockerfile";
}

// ---- worker ----------------------------------------------------------------------------
struct Image {
    lv_image_dsc_t dsc = {};
    uint8_t *px = nullptr;
    ~Image()
    {
        heap_caps_free(px);
    }
};

struct Job {
    enum Op { LIST, OPEN } op;
    Source src;
    std::string path;  // relative to the source root
    uint32_t serial;
};

struct Result {
    uint32_t serial = 0;
    Job::Op op = Job::LIST;
    std::string error;
    std::vector<Entry> entries;
    std::shared_ptr<Image> image;
    std::string text;
    std::string name;
    bool truncated = false;
};

QueueHandle_t jobs = nullptr;
std::mutex result_lock;
std::unique_ptr<Result> ready;
std::atomic<uint32_t> serial{0};

struct {
    smb2_context *ctx = nullptr;
    int share = -1;
} smb;

void smb_close()
{
    if (!smb.ctx) {
        return;
    }
    smb2_disconnect_share(smb.ctx);
    smb2_destroy_context(smb.ctx);
    smb.ctx = nullptr;
    smb.share = -1;
}

bool smb_connect(int index, std::string *error)
{
    if (smb.ctx && smb.share == index) {
        return true;
    }
    smb_close();
    Share s;
    {
        std::lock_guard<std::mutex> g(cfg_lock);
        if (index < 0 || index >= int(shares.size())) {
            *error = "That share is no longer set up.";
            return false;
        }
        s = shares[size_t(index)];
    }
    smb.ctx = smb2_init_context();
    if (!smb.ctx) {
        *error = "Out of memory.";
        return false;
    }
    smb2_set_timeout(smb.ctx, 5);  // a NAS that does not answer fails in seconds, not half a minute
    smb2_set_security_mode(smb.ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (!s.user.empty()) {
        smb2_set_user(smb.ctx, s.user.c_str());
    }
    if (!s.password.empty()) {
        smb2_set_password(smb.ctx, s.password.c_str());
    }
    std::fill(s.password.begin(), s.password.end(), '\0');
    ESP_LOGI(TAG, "SMB: connecting to %s/%s", s.host.c_str(), s.share.c_str());
    const int64_t t0 = esp_timer_get_time();
    const int rc = smb2_connect_share(smb.ctx, s.host.c_str(), s.share.c_str(), s.user.empty() ? nullptr : s.user.c_str());
    ESP_LOGI(TAG, "SMB: connect returned %d after %d ms", rc, int((esp_timer_get_time() - t0) / 1000));
    if (rc < 0) {
        const std::string why = smb2_get_error(smb.ctx);
        *error = why.find("Timeout") != std::string::npos
                 ? "The NAS doesn't answer from the board's network (" + s.host + "). A router or firewall may block it."
                 : "Could not open the share: " + why;
        ESP_LOGW(TAG, "SMB %s/%s: %s", s.host.c_str(), s.share.c_str(), smb2_get_error(smb.ctx));
        smb2_destroy_context(smb.ctx);
        smb.ctx = nullptr;
        return false;
    }
    smb.share = index;
    ESP_LOGI(TAG, "SMB connected: %s/%s", s.host.c_str(), s.share.c_str());
    return true;
}

std::string join(const std::string &a, const std::string &b)
{
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    return a.back() == '/' ? a + b : a + "/" + b;
}

std::string smb_path(const Source &src, const std::string &path)
{
    std::string p = join(src.root, path);
    while (!p.empty() && p.front() == '/') {
        p.erase(p.begin());
    }
    return p;
}

bool list_dir(const Source &src, const std::string &path, std::vector<Entry> &out, std::string *error)
{
    if (src.kind == Kind::Local) {
        DIR *d = opendir(join(src.root, path).c_str());
        if (!d) {
            *error = "This folder cannot be read.";
            return false;
        }
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') {
                continue;
            }
            Entry en;
            en.name = e->d_name;
            struct stat st;
            const std::string full = join(join(src.root, path), en.name);
            if (stat(full.c_str(), &st) == 0) {
                en.dir = S_ISDIR(st.st_mode);
                en.size = uint64_t(st.st_size);
                en.mtime = uint32_t(st.st_mtime);
            } else {
                en.dir = e->d_type == DT_DIR;
            }
            out.push_back(en);
        }
        closedir(d);
        return true;
    }
    std::lock_guard<std::recursive_mutex> g(smb_lock);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!smb_connect(src.share, error)) {
            return false;
        }
        const int64_t t0 = esp_timer_get_time();
        smb2dir *d = smb2_opendir(smb.ctx, smb_path(src, path).c_str());
        ESP_LOGI(TAG, "SMB: opendir \"%s\" %s after %d ms", smb_path(src, path).c_str(), d ? "ok" : smb2_get_error(smb.ctx),
                 int((esp_timer_get_time() - t0) / 1000));
        if (!d) {
            *error = std::string("This folder cannot be read: ") + smb2_get_error(smb.ctx);
            smb_close();  // the connection may have gone stale: one fresh try
            continue;
        }
        while (smb2dirent *e = smb2_readdir(smb.ctx, d)) {
            if (!e->name || e->name[0] == '.') {
                continue;
            }
            out.push_back({e->name, e->st.smb2_type == SMB2_TYPE_DIRECTORY, e->st.smb2_size, uint32_t(e->st.smb2_mtime)});
        }
        smb2_closedir(smb.ctx, d);
        error->clear();
        return true;
    }
    return false;
}

// Up to max bytes of a file (truncated is set when there was more).
bool read_file(const Source &src, const std::string &path, size_t max, std::vector<uint8_t> &out, bool *truncated,
               std::string *error)
{
    *truncated = false;
    if (src.kind == Kind::Local) {
        FILE *f = fopen(join(src.root, path).c_str(), "rb");
        if (!f) {
            *error = "The file cannot be opened.";
            return false;
        }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        const size_t n = std::min<size_t>(size > 0 ? size_t(size) : 0, max);
        *truncated = size_t(size) > max;
        out.resize(n);
        const bool ok = fread(out.data(), 1, n, f) == n;
        fclose(f);
        if (!ok) {
            *error = "The file could not be read.";
        }
        return ok;
    }
    std::lock_guard<std::recursive_mutex> g(smb_lock);
    if (!smb_connect(src.share, error)) {
        return false;
    }
    smb2fh *fh = smb2_open(smb.ctx, smb_path(src, path).c_str(), O_RDONLY);
    if (!fh) {
        *error = std::string("The file cannot be opened: ") + smb2_get_error(smb.ctx);
        smb_close();
        return false;
    }
    smb2_stat_64 st = {};
    smb2_fstat(smb.ctx, fh, &st);
    const size_t n = size_t(std::min<uint64_t>(st.smb2_size, max));
    *truncated = st.smb2_size > max;
    out.resize(n);
    size_t got = 0;
    const uint32_t chunk = std::max<uint32_t>(4096, std::min<uint32_t>(smb2_get_max_read_size(smb.ctx), 256 * 1024));
    while (got < n) {
        const int r = smb2_read(smb.ctx, fh, out.data() + got, uint32_t(std::min<size_t>(chunk, n - got)));
        if (r <= 0) {
            break;
        }
        got += size_t(r);
    }
    smb2_close(smb.ctx, fh);
    if (got < n) {
        *error = "The file could not be read in full.";
        return false;
    }
    return true;
}

// ---- image decoding (as Swifty's chat images, with a decoder pre-scale for big photos) ----
std::shared_ptr<Image> fit(const uint8_t *src, int w, int h, int bpp)
{
    const float k = std::min({1.f, float(VIEW) / w, float(VIEW) / h});
    const int ow = std::max(1, int(w * k)), oh = std::max(1, int(h * k));
    auto img = std::make_shared<Image>();
    img->px = static_cast<uint8_t *>(heap_caps_malloc(size_t(ow) * oh * bpp, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!img->px) {
        return nullptr;
    }
    for (int y = 0; y < oh; ++y) {
        const int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; ++x) {
            const int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 3)
                for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 3, ++n) {
                    const uint8_t *p = src + (size_t(yy) * w + xx) * bpp;
                    if (bpp == 2) {
                        const uint16_t v = uint16_t(p[0] | p[1] << 8);
                        acc[0] += v >> 11, acc[1] += (v >> 5) & 63, acc[2] += v & 31;
                    } else {
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += p[c];
                        }
                    }
                }
            uint8_t *o = img->px + (size_t(y) * ow + x) * bpp;
            if (bpp == 2) {
                const uint16_t v = uint16_t((acc[0] / n) << 11 | (acc[1] / n) << 5 | (acc[2] / n));
                o[0] = uint8_t(v), o[1] = uint8_t(v >> 8);
            } else {
                for (int c = 0; c < 4; ++c) {
                    o[c] = uint8_t(acc[c] / n);
                }
            }
        }
    }
    img->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img->dsc.header.cf = bpp == 2 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;
    img->dsc.header.w = uint32_t(ow);
    img->dsc.header.h = uint32_t(oh);
    img->dsc.header.stride = uint32_t(ow * bpp);
    img->dsc.data = img->px;
    img->dsc.data_size = uint32_t(ow * oh * bpp);
    return img;
}

std::shared_ptr<Image> decode_jpeg(const std::vector<uint8_t> &data, std::string *error)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t header = {};
    // The size first, to let the decoder scale big photos down (to 1/8).
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return nullptr;
    }
    io.inbuf = const_cast<uint8_t *>(data.data());
    io.inbuf_len = int(data.size());
    const bool parsed = jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK;
    jpeg_dec_close(dec);
    if (!parsed || !header.width || !header.height) {
        *error = "This JPEG cannot be read.";
        return nullptr;
    }
    int w = header.width, h = header.height;
    if (w > 2 * VIEW || h > 2 * VIEW) {
        const float k = std::max({float(VIEW) / w, float(VIEW) / h, 1.0f / 8});
        const float kk = std::max(k, std::max(8.0f / w, 8.0f / h));
        cfg.scale.width = uint16_t(std::max(8, int(w * std::min(1.0f, std::max(kk, float(VIEW) / std::max(w, h)))) / 8 * 8));
        cfg.scale.height = uint16_t(std::max(8, int(h * float(cfg.scale.width) / w) / 8 * 8));
        if (cfg.scale.width * 8 < w || cfg.scale.height * 8 < h) { // the decoder's limit
            cfg.scale.width = uint16_t((w / 8 + 7) / 8 * 8), cfg.scale.height = uint16_t((h / 8 + 7) / 8 * 8);
        }
        w = cfg.scale.width, h = cfg.scale.height;
    } else if (size_t(w) * h > 2048u * 2048u) {
        *error = "This image is too large.";
        return nullptr;
    }
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return nullptr;
    }
    io = {};
    io.inbuf = const_cast<uint8_t *>(data.data());
    io.inbuf_len = int(data.size());
    int outlen = 0;
    std::shared_ptr<Image> img;
    if (jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK && jpeg_dec_get_outbuf_len(dec, &outlen) == JPEG_ERR_OK) {
        auto *raw = static_cast<uint8_t *>(jpeg_calloc_align(size_t(outlen), 16));
        if (raw) {
            io.outbuf = raw;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                img = fit(raw, w, h, 2);
            } else {
                *error = "This JPEG could not be decoded.";
            }
            jpeg_free_align(raw);
        } else {
            *error = "Not enough memory for this image.";
        }
    }
    jpeg_dec_close(dec);
    return img;
}

std::shared_ptr<Image> decode_png(const std::vector<uint8_t> &data, std::string *error)
{
    png_image png = {};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, data.data(), data.size())) {
        *error = "This PNG cannot be read.";
        return nullptr;
    }
    if (png.width * png.height > 1600 * 1600) {
        png_image_free(&png);
        *error = "This PNG is too large to show here.";
        return nullptr;
    }
    png.format = PNG_FORMAT_BGRA;  // LVGL ARGB8888 in memory
    const size_t bytes = PNG_IMAGE_SIZE(png);
    auto *raw = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    std::shared_ptr<Image> img;
    if (raw && png_image_finish_read(&png, nullptr, raw, 0, nullptr)) {
        img = fit(raw, int(png.width), int(png.height), 4);
    } else {
        *error = "This PNG could not be decoded.";
    }
    heap_caps_free(raw);
    png_image_free(&png);
    return img;
}

void run(const Job &job)
{
    auto r = std::make_unique<Result>();
    r->serial = job.serial;
    r->op = job.op;
    if (job.op == Job::LIST) {
        if (job.src.kind == Kind::Local && !mosaico_storage_mount()) {
            r->error = mosaico_storage_status();
        } else if (list_dir(job.src, job.path, r->entries, &r->error)) {
            std::sort(r->entries.begin(), r->entries.end(), [](const Entry & a, const Entry & b) {
                if (a.dir != b.dir) {
                    return a.dir;
                }
                return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
            });
        }
    } else {
        const std::string name = job.path.substr(job.path.find_last_of('/') + 1);
        r->name = name;
        std::vector<uint8_t> data;
        const bool image = image_name(name);
        if (read_file(job.src, job.path, image ? MAX_IMAGE_FILE : MAX_TEXT, data, &r->truncated, &r->error)) {
            if (image && r->truncated) {
                r->error = "This image is larger than 12 MB.";
            } else if (image) {
                r->image = data.size() > 3 && data[0] == 0xFF && data[1] == 0xD8 ? decode_jpeg(data, &r->error)
                           : decode_png(data, &r->error);
                if (!r->image && r->error.empty()) {
                    r->error = "Not enough memory for this image.";
                }
            } else {
                r->text.assign(data.begin(), data.end());
                // Show text only: NULs and other control bytes become dots.
                for (char &c : r->text)
                    if (c == '\0' || (uint8_t(c) < 32 && c != '\n' && c != '\t' && c != '\r')) {
                        c = '.';
                    }
            }
        }
    }
    if (job.op == Job::LIST)
        ESP_LOGI(TAG, "list \"%s\": %s, %u entries", job.path.c_str(), r->error.empty() ? "ok" : r->error.c_str(),
                 unsigned(r->entries.size()));
    std::lock_guard<std::mutex> g(result_lock);
    ready = std::move(r);
}

void worker(void *)
{
    for (;;) {
        Job *job = nullptr;
        if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(60000)) != pdTRUE) {
            std::lock_guard<std::recursive_mutex> g(smb_lock);
            smb_close();  // idle for a minute: let the share go
            continue;
        }
        if (job->serial == serial.load()) {
            run(*job);    // skip jobs already superseded
        }
        delete job;
    }
}

void post(Job::Op op, const Source &src, const std::string &path)
{
    auto *job = new Job{op, src, path, ++serial};
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        delete job;
    }
}

// ---- photos on the NAS (the camera's store) ----------------------------------------------------
// The photos share: the one named in "photos", else the first share.
int photo_share()
{
    std::lock_guard<std::mutex> g(cfg_lock);
    if (shares.empty()) {
        return -1;
    }
    for (size_t i = 0; i < shares.size(); ++i)
        if (!photo_dest.share.empty() && (photo_dest.share == shares[i].name || photo_dest.share == shares[i].share)) {
            return int(i);
        }
    return photo_dest.share.empty() ? 0 : -1;
}

std::string photo_folder()
{
    std::lock_guard<std::mutex> g(cfg_lock);
    std::string p = photo_dest.path.empty() ? DEFAULT_PHOTO_PATH : photo_dest.path;
    while (!p.empty() && (p.front() == '/' || p.front() == '\\')) {
        p.erase(p.begin());
    }
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) {
        p.pop_back();
    }
    return p;
}

bool safe_name(const char *name)
{
    if (!name || !name[0] || strlen(name) > 64) {
        return false;
    }
    // Any file name the folder holds (spaces too), but never a path.
    for (const char *c = name; *c; ++c)
        if (*c == '/' || *c == '\\' || uint8_t(*c) < 0x20) {
            return false;
        }
    return name[0] != '.';
}

// Connected to the photos share (smb_lock held); creates the folder once.
bool nas_open(std::string *folder)
{
    const int share = photo_share();
    std::string error;
    if (share < 0 || !smb_connect(share, &error)) {
        return false;
    }
    *folder = photo_folder();
    static int made_for = -1;
    if (made_for != share) {
        std::string so_far;
        for (size_t start = 0; start <= folder->size();) {
            const size_t slash = folder->find('/', start);
            so_far = folder->substr(0, slash == std::string::npos ? folder->size() : slash);
            if (!so_far.empty()) {
                smb2_mkdir(smb.ctx, so_far.c_str());    // fails harmlessly when it exists
            }
            if (slash == std::string::npos) {
                break;
            }
            start = slash + 1;
        }
        made_for = share;
    }
    return true;
}
}  // namespace

extern "C" bool files_nas_ready(void)
{
    return photo_share() >= 0;
}

extern "C" bool files_nas_put(const char *name, const uint8_t *data, size_t size)
{
    if (!safe_name(name) || !data) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> g(smb_lock);
    std::string folder;
    if (!nas_open(&folder)) {
        return false;
    }
    const std::string path = folder.empty() ? name : folder + "/" + name;
    smb2fh *fh = smb2_open(smb.ctx, path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!fh) {
        ESP_LOGW(TAG, "NAS: cannot create %s: %s", path.c_str(), smb2_get_error(smb.ctx));
        smb_close();
        return false;
    }
    size_t sent = 0;
    const uint32_t chunk = std::max<uint32_t>(4096, std::min<uint32_t>(smb2_get_max_write_size(smb.ctx), 64 * 1024));
    while (sent < size) {
        const int w = smb2_write(smb.ctx, fh, data + sent, uint32_t(std::min<size_t>(chunk, size - sent)));
        if (w <= 0) {
            break;
        }
        sent += size_t(w);
    }
    smb2_close(smb.ctx, fh);
    if (sent < size) {
        ESP_LOGW(TAG, "NAS: %s cut short (%u of %u bytes)", path.c_str(), unsigned(sent), unsigned(size));
        smb2_unlink(smb.ctx, path.c_str());
        return false;
    }
    ESP_LOGI(TAG, "NAS: saved %s (%u bytes)", path.c_str(), unsigned(size));
    return true;
}

extern "C" int files_nas_list(files_photo_t *out, int max)
{
    std::lock_guard<std::recursive_mutex> g(smb_lock);
    std::string folder;
    if (!nas_open(&folder)) {
        return -1;
    }
    smb2dir *d = smb2_opendir(smb.ctx, folder.c_str());
    if (!d) {
        smb_close();
        return -1;
    }
    int n = 0;
    while (smb2dirent *e = smb2_readdir(smb.ctx, d)) {
        if (n >= max) {
            break;
        }
        if (!e->name || e->st.smb2_type == SMB2_TYPE_DIRECTORY || !jpeg_name(e->name)) {
            continue;    // the Gallery decodes JPEG
        }
        if (strlen(e->name) >= sizeof(out[n].name) || e->name[0] == '.') {
            continue;    // would not fit: never a cut name
        }
        strlcpy(out[n].name, e->name, sizeof(out[n].name));
        out[n].size = uint32_t(e->st.smb2_size);
        out[n].mtime = uint32_t(e->st.smb2_mtime);
        ++n;
    }
    smb2_closedir(smb.ctx, d);
    return n;
}

extern "C" bool files_nas_get(const char *name, uint8_t **data, size_t *size)
{
    if (!safe_name(name)) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> g(smb_lock);
    std::string folder;
    if (!nas_open(&folder)) {
        return false;
    }
    const std::string path = folder.empty() ? name : folder + "/" + name;
    smb2fh *fh = smb2_open(smb.ctx, path.c_str(), O_RDONLY);
    if (!fh) {
        smb_close();
        return false;
    }
    smb2_stat_64 st = {};
    smb2_fstat(smb.ctx, fh, &st);
    const size_t n = size_t(std::min<uint64_t>(st.smb2_size, MAX_IMAGE_FILE));
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(std::max<size_t>(n, 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t got = 0;
    while (buf && got < n) {
        const int r = smb2_read(smb.ctx, fh, buf + got, uint32_t(std::min<size_t>(64 * 1024, n - got)));
        if (r <= 0) {
            break;
        }
        got += size_t(r);
    }
    smb2_close(smb.ctx, fh);
    if (!buf || got < n || !n) {
        heap_caps_free(buf);
        return false;
    }
    *data = buf;
    *size = n;
    return true;
}

extern "C" bool files_nas_delete(const char *name)
{
    if (!safe_name(name)) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> g(smb_lock);
    std::string folder;
    if (!nas_open(&folder)) {
        return false;
    }
    return smb2_unlink(smb.ctx, (folder.empty() ? std::string(name) : folder + "/" + name).c_str()) == 0;
}

namespace {
// ---- UI ----------------------------------------------------------------------------------
struct {
    files_hooks_t hooks{};
    bool built = false, active = false;
    lv_obj_t *screen = nullptr, *title = nullptr, *subtitle = nullptr, *list = nullptr, *message = nullptr;
    lv_obj_t *viewer = nullptr, *viewer_title = nullptr, *viewer_body = nullptr;
    std::vector<Source> sources;
    int source = -1;      // -1: the list of sources
    std::string path;     // relative to the source root
    std::vector<Entry> entries;
    std::shared_ptr<Image> image;  // shown in the viewer
    uint32_t waiting = 0;
} U;

std::string human(uint64_t n)
{
    char t[24];
    if (n < 1024) {
        snprintf(t, sizeof(t), "%u B", unsigned(n));
    } else if (n < 1024 * 1024) {
        snprintf(t, sizeof(t), "%.0f KB", double(n) / 1024);
    } else if (n < 1024ull * 1024 * 1024) {
        snprintf(t, sizeof(t), "%.1f MB", double(n) / 1048576);
    } else {
        snprintf(t, sizeof(t), "%.1f GB", double(n) / 1073741824.0);
    }
    return t;
}

std::vector<Source> make_sources()
{
    std::vector<Source> out;
    // (photo_share and photo_folder take cfg_lock themselves: not under it.)
    const int dest = photo_share();
    if (dest >= 0) {
        const std::string folder = photo_folder();
        std::string host, share;
        {
            std::lock_guard<std::mutex> g(cfg_lock);
            if (dest < int(shares.size())) {
                host = shares[size_t(dest)].host, share = shares[size_t(dest)].share;
            }
        }
        out.push_back({Kind::Smb, "Photos", "Camera photos \xC2\xB7 \\\\" + host + "\\" + share + "\\" + folder, folder, dest, true});
    }
    if (mosaico_storage_ready())
        out.push_back({Kind::Local, "This board", mosaico_storage_status(), MOSAICO_STORAGE_ROOT, -1, false});
    std::lock_guard<std::mutex> g(cfg_lock);
    for (size_t i = 0; i < shares.size(); ++i)
        out.push_back({Kind::Smb, shares[i].name.empty() ? shares[i].share : shares[i].name,
                       "\\\\" + shares[i].host + "\\" + shares[i].share, shares[i].path, int(i), false});
    return out;
}

void build_sources()
{
    U.sources = make_sources();
}

void render();
void open_viewer();

void row_cb(lv_event_t *e)
{
    const int i = int(intptr_t(lv_event_get_user_data(e)));
    if (U.source < 0) {
        if (i < 0 || i >= int(U.sources.size())) {
            return;
        }
        U.source = i;
        U.path.clear();
        U.entries.clear();
        U.waiting = serial + 1;
        post(Job::LIST, U.sources[size_t(i)], U.path);
        render();
        return;
    }
    if (i < 0 || i >= int(U.entries.size())) {
        return;
    }
    const Entry en = U.entries[size_t(i)];
    if (en.dir) {
        U.path = join(U.path, en.name);
        U.entries.clear();
        U.waiting = serial + 1;
        post(Job::LIST, U.sources[size_t(U.source)], U.path);
        render();
    } else if (image_name(en.name) || text_name(en.name) || en.size < 4096) {
        U.waiting = serial + 1;
        post(Job::OPEN, U.sources[size_t(U.source)], join(U.path, en.name));
        open_viewer();
        lv_label_set_text(U.viewer_title, en.name.c_str());
        lv_obj_clean(U.viewer_body);
        label(U.viewer_body, "Opening\xE2\x80\xA6", 0, 180, 440, TEXT2, 18, true, LV_TEXT_ALIGN_CENTER);
    }
}

void close_viewer()
{
    if (!U.viewer) {
        return;
    }
    lv_obj_add_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(U.viewer_body);
    U.image.reset();
}

void back_cb(lv_event_t *)
{
    if (U.viewer && !lv_obj_has_flag(U.viewer, LV_OBJ_FLAG_HIDDEN)) {
        ++serial;  // drop an open still in progress
        close_viewer();
        return;
    }
    if (U.source < 0) {
        if (U.hooks.exit) {
            U.hooks.exit();
        }
        return;
    }
    if (U.path.empty()) {
        U.source = -1;
        build_sources();
        render();
        return;
    }
    const size_t slash = U.path.find_last_of('/');
    U.path = slash == std::string::npos ? "" : U.path.substr(0, slash);
    U.entries.clear();
    U.waiting = serial + 1;
    post(Job::LIST, U.sources[size_t(U.source)], U.path);
    render();
}

lv_obj_t *row(int y, const char *ic, const char *title, const std::string &detail, int index)
{
    lv_obj_t *r = box(U.list, 0, y, 404, 58, 18);
    lv_obj_set_style_bg_color(r, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_add_flag(r, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    press_fx(r);
    icon(r, ic, 14, 15, 28, ACC.glow, 1.7f);
    lv_obj_t *t = label(r, title, 54, detail.empty() ? 17 : 8, 330, TEXT, 17, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t, 23);
    if (!detail.empty()) {
        lv_obj_t *d = label(r, detail.c_str(), 54, 32, 330, TEXT3, 13);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(d, 18);
    }
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(index)));
    return r;
}

void render()
{
    lv_obj_clean(U.list);
    lv_obj_scroll_to_y(U.list, 0, LV_ANIM_OFF);
    lv_obj_add_flag(U.message, LV_OBJ_FLAG_HIDDEN);
    if (U.source < 0) {
        lv_label_set_text(U.title, "Files");
        lv_label_set_text(U.subtitle, "The board and your shares");
        int y = 0;
        for (size_t i = 0; i < U.sources.size(); ++i, y += 64) {
            const Source &s = U.sources[i];
            row(y, s.kind == Kind::Smb ? "nas" : i == 0 ? "image" : "folder", s.name.c_str(), s.detail, int(i));
        }
        if (U.sources.size() <= 2) {
            lv_obj_t *n = label(U.list, "Add network shares with tools/mosaico-config.py (\"files\" section).", 10, y + 6,
                                384, TEXT3, 14);
            wrap_label(n);
        }
        return;
    }
    const Source &s = U.sources[size_t(U.source)];
    const std::string where = U.path.empty() ? s.name : U.path.substr(U.path.find_last_of('/') + 1);
    lv_label_set_text(U.title, where.c_str());
    lv_label_set_text(U.subtitle, (s.name + (U.path.empty() ? "" : " / " + U.path)).c_str());
    if (U.waiting) {
        lv_label_set_text(lv_obj_get_child(U.message, 0), "Loading\xE2\x80\xA6");
        lv_obj_remove_flag(U.message, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (U.entries.empty()) {
        lv_label_set_text(lv_obj_get_child(U.message, 0), "Nothing here.");
        lv_obj_remove_flag(U.message, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const int shown = std::min<int>(int(U.entries.size()), 200);
    for (int i = 0; i < shown; ++i) {
        const Entry &e = U.entries[size_t(i)];
        std::string detail;
        if (!e.dir) {
            detail = human(e.size);
            if (e.mtime > 1000000000) {
                char when[24];
                const time_t t = e.mtime;
                struct tm local;
                localtime_r(&t, &local);
                strftime(when, sizeof(when), " \xC2\xB7 %d %b %Y", &local);
                detail += when;
            }
        }
        row(i * 64, e.dir ? "folder" : image_name(e.name) ? "image" : "file", e.name.c_str(), detail, i);
    }
    if (int(U.entries.size()) > shown) {
        label(U.list, "Only the first 200 are shown.", 10, shown * 64 + 4, 380, TEXT3, 14);
    }
}

void open_viewer()
{
    lv_obj_remove_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(U.viewer);
}

void show_result(Result &r)
{
    if (r.op == Job::LIST) {
        U.waiting = 0;
        U.entries = std::move(r.entries);
        render();
        if (!r.error.empty()) {
            lv_label_set_text(lv_obj_get_child(U.message, 0), r.error.c_str());
            lv_obj_remove_flag(U.message, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    U.waiting = 0;
    if (!U.viewer || lv_obj_has_flag(U.viewer, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_clean(U.viewer_body);
    if (!r.error.empty()) {
        lv_obj_t *l = label(U.viewer_body, r.error.c_str(), 20, 170, 400, WARN, 17, true, LV_TEXT_ALIGN_CENTER);
        wrap_label(l);
        return;
    }
    if (r.image) {
        U.image = r.image;
        lv_obj_t *img = lv_image_create(U.viewer_body);
        lv_image_set_src(img, &U.image->dsc);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    lv_obj_t *t = label(U.viewer_body, "", 8, 0, 424, TEXT, 15);
    wrap_label(t);
    lv_label_set_text(t, r.text.empty() ? "(empty)" : r.text.c_str());
    if (r.truncated) {
        lv_obj_update_layout(t);
        label(U.viewer_body, "Only the first 32 KB is shown.", 8, lv_obj_get_height(t) + 12, 424, TEXT3, 13);
    }
}

void poll(lv_timer_t *)
{
    std::unique_ptr<Result> r;
    {
        std::lock_guard<std::mutex> g(result_lock);
        r = std::move(ready);
    }
    if (r && r->serial == serial.load()) {
        show_result(*r);
    }
}

void build(lv_obj_t *s)
{
    U.screen = s;
    lv_obj_t *back = box(s, 56, 14, 44, 44, 22);
    lv_obj_set_style_bg_color(back, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    icon(back, "back", 10, 10, 24, TEXT, 2.0f);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    press_fx(back);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);
    U.title = label(s, "Files", 112, 12, 300, TEXT, 24, true);
    lv_label_set_long_mode(U.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(U.title, 31);
    U.subtitle = label(s, "", 112, 44, 320, TEXT3, 13);
    lv_label_set_long_mode(U.subtitle, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(U.subtitle, 18);
    U.list = box(s, 38, 74, 404, 380);
    lv_obj_add_flag(U.list, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM));
    lv_obj_set_scroll_dir(U.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.list, LV_SCROLLBAR_MODE_OFF);
    U.message = box(s, 40, 200, 400, 80);
    label(U.message, "", 0, 0, 400, TEXT2, 17, true, LV_TEXT_ALIGN_CENTER);
    wrap_label(lv_obj_get_child(U.message, 0));
    lv_obj_add_flag(U.message, LV_OBJ_FLAG_HIDDEN);
    // The preview, over everything.
    U.viewer = box(s, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(U.viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.viewer, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.viewer, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    U.viewer_body = box(U.viewer, 20, 64, 440, 396);
    lv_obj_add_flag(U.viewer_body, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM));
    lv_obj_set_scroll_dir(U.viewer_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.viewer_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *vb = box(U.viewer, 56, 12, 44, 44, 22);
    lv_obj_set_style_bg_color(vb, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(vb, LV_OPA_COVER, 0);
    icon(vb, "back", 10, 10, 24, TEXT, 2.0f);
    lv_obj_add_flag(vb, LV_OBJ_FLAG_CLICKABLE);
    press_fx(vb);
    lv_obj_add_event_cb(vb, back_cb, LV_EVENT_CLICKED, nullptr);
    U.viewer_title = label(U.viewer, "", 112, 22, 320, TEXT, 18, true);
    lv_label_set_long_mode(U.viewer_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(U.viewer_title, 24);
    lv_timer_create(poll, 80, nullptr);
    U.built = true;
}
}  // namespace

// ESP-Iris 0x4744/50, a network check from the board: payload "host:port"
// (an IPv4 address); replies whether a TCP connection opens, and how fast.
esp_err_t tcp_probe_rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    char target[64] = {};
    memcpy(target, request->payload, std::min<size_t>(request->payload_size, sizeof(target) - 1));
    char *colon = strchr(target, ':');
    if (!colon) {
        return ESP_ERR_INVALID_ARG;
    }
    *colon = 0;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(atoi(colon + 1)));
    if (inet_pton(AF_INET, target, &addr.sin_addr) != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv = {4, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int64_t start = esp_timer_get_time();
    const int r = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    const int e = errno;
    close(fd);
    const int n = snprintf(reinterpret_cast<char *>(out), capacity, "%s:%s %s after %d ms (errno %d)", target, colon + 1,
                           r == 0 ? "open" : "failed", int((esp_timer_get_time() - start) / 1000), r == 0 ? 0 : e);
    *size = n < 0 ? 0 : std::min<size_t>(size_t(n), capacity);
    return ESP_OK;
}

// ESP-Iris 0x4744/51, the Files app's SMB path from the Mac: payload byte
// share index, then a folder path; replies the first entries or the error.
// Runs on the RPC task, so it is quick: the 5 s SMB timeout bounds it.
esp_err_t smb_list_rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    if (request->payload_size < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    // On the Files worker (an SMB call can take long): the result is logged.
    Source src{Kind::Smb, "probe", "", "", int(request->payload[0]), false};
    const std::string path(reinterpret_cast<const char *>(request->payload + 1), request->payload_size - 1);
    auto *job = new Job{Job::LIST, src, path, ++serial};
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        delete job;
    }
    const char *reply = "queued; see the log";
    *size = std::min(capacity, strlen(reply));
    memcpy(out, reply, *size);
    return ESP_OK;
}

extern "C" void files_app_init(const files_hooks_t *hooks)
{
    esp_iris_rpc_register(0x4744, 50, tcp_probe_rpc, nullptr);
    esp_iris_rpc_register(0x4744, 51, smb_list_rpc, nullptr);
    if (hooks) {
        U.hooks = *hooks;
    }
    load_shares();
    if (!jobs) {
        jobs = xQueueCreate(4, sizeof(Job *));
        // Network and NAND I/O only (no NVS or SPI-flash access), so the stack
        // can live in PSRAM.
        xTaskCreatePinnedToCoreWithCaps(worker, "files", 16 * 1024, nullptr, 3, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

extern "C" void files_app_open(lv_obj_t *screen)
{
    if (!U.built) {
        build(screen);
    }
    U.active = true;
    if (U.source < 0) {
        build_sources();
        render();
    }
}

extern "C" void files_app_set_active(bool active)
{
    U.active = active && U.built;
}

extern "C" const char *files_app_config_apply(const char *json, size_t length)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, length) || !doc.is<JsonObject>()) {
        return "expected an object";
    }
    for (JsonPair kv : doc.as<JsonObject>())
        if (strcmp(kv.key().c_str(), "shares") && strcmp(kv.key().c_str(), "photos")) {
            return "unknown key (use shares, photos)";
        }
    if (!doc["shares"].is<JsonArray>()) {
        return "shares must be a list";
    }
    JsonDocument clean;
    JsonArray out = clean["shares"].to<JsonArray>();
    if (!doc["photos"].isNull()) {
        if (!doc["photos"].is<JsonObject>()) {
            return "photos must be an object with share and path";
        }
        for (JsonPair kv : doc["photos"].as<JsonObject>()) {
            if (strcmp(kv.key().c_str(), "share") && strcmp(kv.key().c_str(), "path")) {
                return "unknown photos key (use share, path)";
            }
            if (!kv.value().is<const char *>()) {
                return "photos values must be strings";
            }
        }
        clean["photos"]["share"] = doc["photos"]["share"] | "";
        clean["photos"]["path"] = doc["photos"]["path"] | "";
    }
    static const char *const KEYS[] = {"name", "host", "share", "user", "password", "path"};
    for (JsonObjectConst s : doc["shares"].as<JsonArrayConst>()) {
        for (JsonPairConst kv : s) {
            bool known = false;
            for (const char *k : KEYS) {
                known |= !strcmp(k, kv.key().c_str());
            }
            if (!known) {
                return "unknown share key (use name, host, share, user, password, path)";
            }
            if (!kv.value().is<const char *>()) {
                return "share values must be strings";
            }
        }
        if (!(s["host"] | "")[0] || !(s["share"] | "")[0]) {
            return "each share needs host and share";
        }
        JsonObject o = out.add<JsonObject>();
        for (const char *k : KEYS) {
            o[k] = s[k] | "";
        }
        if (s["password"].isNull()) {
            // No password given: the one saved for this share (same name, or
            // else the same host and share), as the web settings send it.
            std::lock_guard<std::mutex> g(cfg_lock);
            const Share *same = nullptr;
            for (const Share &old : shares) {
                if (old.name == (s["name"] | "") && *(s["name"] | "")) {
                    same = &old;
                    break;
                }
                if (!same && old.host == (s["host"] | "") && old.share == (s["share"] | "")) {
                    same = &old;
                }
            }
            if (same) {
                o["password"] = same->password;
            }
        }
    }
    std::string text;
    serializeJson(clean, text);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return "storage unavailable";
    }
    esp_err_t err = out.size() ? nvs_set_str(h, "shares", text.c_str()) : nvs_erase_key(h, "shares");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    std::fill(text.begin(), text.end(), '\0');
    if (err != ESP_OK) {
        return "storage write failed";
    }
    load_shares();
    {
        std::lock_guard<std::recursive_mutex> g(smb_lock);
        smb_close();  // reconnect with the new settings
    }
    return nullptr;
}

extern "C" void files_app_config_status(char *out, size_t capacity)
{
    JsonDocument doc;
    JsonArray list = doc["shares"].to<JsonArray>();
    {
        std::lock_guard<std::mutex> g(cfg_lock);
        for (const Share &s : shares) {
            JsonObject o = list.add<JsonObject>();
            o["name"] = s.name;
            o["host"] = s.host;
            o["share"] = s.share;
            o["user"] = s.user;
            o["path"] = s.path;
            o["password"] = !s.password.empty();
        }
    }
    {
        std::lock_guard<std::mutex> g(cfg_lock);
        doc["photos"]["share"] = photo_dest.share;
        doc["photos"]["path"] = photo_dest.path.empty() ? DEFAULT_PHOTO_PATH : photo_dest.path;
    }
    serializeJson(doc, out, capacity);
}

extern "C" void files_app_summary(files_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    out->storage = mosaico_storage_ready();
    {
        std::lock_guard<std::mutex> g(cfg_lock);
        out->shares = int(shares.size());
    }
    strlcpy(out->status, mosaico_storage_status(), sizeof(out->status));
}

// ---- Swifty's tools -------------------------------------------------------------------------
namespace {
// "Source/folder/file" -> the source and the path inside it.
bool resolve(const char *full, Source *src, std::string *path, std::string *error)
{
    std::string p = full ? full : "";
    for (char &c : p)
        if (c == '\\') {
            c = '/';
        }
    while (!p.empty() && p.front() == '/') {
        p.erase(p.begin());
    }
    const size_t slash = p.find('/');
    const std::string head = p.substr(0, slash), rest = slash == std::string::npos ? "" : p.substr(slash + 1);
    for (const Source &s : make_sources()) {
        if (strcasecmp(s.name.c_str(), head.c_str())) {
            continue;
        }
        if (rest.find("..") != std::string::npos) {
            *error = "Paths may not go up a folder.";
            return false;
        }
        *src = s;
        *path = rest;
        return true;
    }
    *error = "No place called \"" + head + "\". Places: ";
    for (const Source &s : make_sources()) {
        *error += s.name + "; ";
    }
    return false;
}
}  // namespace

extern "C" void files_app_tool_list(const char *path, char *out, size_t capacity)
{
    std::string text;
    if (!path || !*path || !strcmp(path, "/")) {
        const std::vector<Source> all = make_sources();
        text = all.empty() ? "No places: no shares set up." : "Places (use as the first part of a path):\n";
        for (const Source &s : all) {
            text += "- " + s.name + ": " + s.detail + "\n";
        }
        strlcpy(out, text.c_str(), capacity);
        return;
    }
    Source src;
    std::string inner, error;
    std::vector<Entry> entries;
    if (!resolve(path, &src, &inner, &error) || !list_dir(src, inner, entries, &error)) {
        strlcpy(out, error.c_str(), capacity);
        return;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry & a, const Entry & b) {
        return a.dir != b.dir ? a.dir : strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    char line[160];
    snprintf(line, sizeof(line), "%s: %u item(s)\n", path, unsigned(entries.size()));
    text = line;
    for (const Entry &e : entries) {
        if (e.dir) {
            snprintf(line, sizeof(line), "- %s/\n", e.name.c_str());
        } else {
            snprintf(line, sizeof(line), "- %s (%llu KB)\n", e.name.c_str(), (unsigned long long)((e.size + 1023) / 1024));
        }
        if (text.size() + strlen(line) + 32 > capacity) {
            text += "- \xE2\x80\xA6 more\n";
            break;
        }
        text += line;
    }
    strlcpy(out, text.c_str(), capacity);
}

extern "C" void files_app_tool_read(const char *path, char *out, size_t capacity)
{
    Source src;
    std::string inner, error;
    std::vector<uint8_t> data;
    bool truncated = false;
    if (!resolve(path, &src, &inner, &error) || inner.empty() ||
            !read_file(src, inner, capacity > 200 ? capacity - 200 : capacity / 2, data, &truncated, &error)) {
        strlcpy(out, error.empty() ? "Name a file inside a place." : error.c_str(), capacity);
        return;
    }
    if (image_name(inner)) {
        snprintf(out, capacity, "%s is an image (%u bytes); open it in Files or the Gallery to see it.", path, unsigned(data.size()));
        return;
    }
    for (uint8_t c : data)
        if (c == 0) {
            snprintf(out, capacity, "%s is not a text file.", path);
            return;
        }
    std::string text(data.begin(), data.end());
    if (truncated) {
        text += "\n[\xE2\x80\xA6 the rest of the file is cut]";
    }
    strlcpy(out, text.c_str(), capacity);
}
