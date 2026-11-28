// SPDX-License-Identifier: Apache-2.0
// The platform under the profiles model (profiles_internal.h): NVS, esp_timer,
// the hardware RNG and PSA's PBKDF2 on the board; memory, std::chrono,
// std::random_device and a portable SHA-256 on the host (the desktop preview
// and the unit tests).
#include "profiles_internal.h"

#include <string.h>

#include <algorithm>
#include <atomic>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "psa/crypto.h"
#else
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#endif

namespace profiles_platform {
namespace {
std::atomic<uint32_t> calibrated{0};
std::atomic<int> self_test{-1};  // -1 not run, 0 failed, 1 passed
}  // namespace

#ifdef ESP_PLATFORM
namespace {
const char *TAG = "profiles";
constexpr const char *NAMESPACE = "profiles";

bool nvs_get(const char *key, void *data, size_t *size)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;  // nothing saved yet
    }
    const esp_err_t err = nvs_get_blob(h, key, data, size);
    nvs_close(h);
    return err == ESP_OK;
}

bool nvs_set(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, key, data, size);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool nvs_erase(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return true;  // no namespace: nothing to erase
    }
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}
}  // namespace

const Store &nvs_store()
{
    static const Store store = {nvs_get, nvs_set, nvs_erase};
    return store;
}

uint32_t now_ms()
{
    return uint32_t(esp_timer_get_time() / 1000);
}

void random_bytes(uint8_t *out, size_t n)
{
    esp_fill_random(out, n);
}

bool pbkdf2_sha256(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[32])
{
    if (psa_crypto_init() != PSA_SUCCESS) {  // a no-op once the system has done it
        return false;
    }
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t s = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (s == PSA_SUCCESS) {
        s = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations);
    }
    if (s == PSA_SUCCESS) {
        s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    }
    if (s == PSA_SUCCESS) {
        s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, password, password_len);
    }
    if (s == PSA_SUCCESS) {
        s = psa_key_derivation_output_bytes(&op, out, 32);
    }
    psa_key_derivation_abort(&op);
    if (s != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PBKDF2 failed (%d)", int(s));
    }
    return s == PSA_SUCCESS;
}

namespace {
int64_t micros()
{
    return esp_timer_get_time();
}
}  // namespace
#else
// ---- host: memory, the steady clock and a portable SHA-256 -------------------------------
namespace {
std::mutex store_lock;
std::map<std::string, std::vector<uint8_t>> &blobs()
{
    static std::map<std::string, std::vector<uint8_t>> m;
    return m;
}

bool mem_get(const char *key, void *data, size_t *size)
{
    std::lock_guard<std::mutex> g(store_lock);
    auto it = blobs().find(key);
    if (it == blobs().end() || it->second.size() > *size) {
        return false;
    }
    memcpy(data, it->second.data(), it->second.size());
    *size = it->second.size();
    return true;
}

bool mem_set(const char *key, const void *data, size_t size)
{
    std::lock_guard<std::mutex> g(store_lock);
    const auto *p = static_cast<const uint8_t *>(data);
    blobs()[key].assign(p, p + size);
    return true;
}

bool mem_erase(const char *key)
{
    std::lock_guard<std::mutex> g(store_lock);
    blobs().erase(key);
    return true;
}

struct Sha256 {
    uint32_t h[8];
    uint8_t block[64];
    uint64_t length = 0;
    size_t used = 0;

    static uint32_t rotr(uint32_t x, int n)
    {
        return (x >> n) | (x << (32 - n));
    }
    void reset()
    {
        static const uint32_t IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
                                      };
        memcpy(h, IV, sizeof(h));
        length = 0;
        used = 0;
    }
    void compress(const uint8_t *p)
    {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
    }
    void update(const uint8_t *p, size_t n)
    {
        length += n;
        while (n) {
            const size_t take = std::min(n, sizeof(block) - used);
            memcpy(block + used, p, take);
            used += take, p += take, n -= take;
            if (used == sizeof(block)) {
                compress(block);
                used = 0;
            }
        }
    }
    void finish(uint8_t out[32])
    {
        const uint64_t bits = length * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) {
            update(&zero, 1);
        }
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) {
            len[i] = uint8_t(bits >> (56 - 8 * i));
        }
        update(len, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = uint8_t(h[i] >> 24), out[i * 4 + 1] = uint8_t(h[i] >> 16);
            out[i * 4 + 2] = uint8_t(h[i] >> 8), out[i * 4 + 3] = uint8_t(h[i]);
        }
    }
};

// HMAC-SHA256 keyed once: the inner and outer states after their pads.
struct Hmac {
    Sha256 inner, outer;
    Hmac(const uint8_t *key, size_t key_len)
    {
        uint8_t k[64] = {};
        if (key_len > 64) {
            Sha256 s;
            s.reset();
            s.update(key, key_len);
            s.finish(k);
        } else {
            memcpy(k, key, key_len);
        }
        uint8_t pad[64];
        for (int i = 0; i < 64; ++i) {
            pad[i] = k[i] ^ 0x36;
        }
        inner.reset();
        inner.update(pad, 64);
        for (int i = 0; i < 64; ++i) {
            pad[i] = k[i] ^ 0x5c;
        }
        outer.reset();
        outer.update(pad, 64);
    }
    void mac(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t out[32]) const
    {
        Sha256 i = inner, o = outer;
        i.update(a, an);
        if (bn) {
            i.update(b, bn);
        }
        uint8_t ih[32];
        i.finish(ih);
        o.update(ih, 32);
        o.finish(out);
    }
};

int64_t micros()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

const Store &nvs_store()
{
    static const Store store = {mem_get, mem_set, mem_erase};
    return store;
}

uint32_t now_ms()
{
    return uint32_t(micros() / 1000);
}

void random_bytes(uint8_t *out, size_t n)
{
    static std::random_device rd;
    for (size_t i = 0; i < n; ++i) {
        out[i] = uint8_t(rd());
    }
}

bool pbkdf2_sha256(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[32])
{
    if (!iterations) {
        return false;
    }
    // One block: a 32-byte result is exactly SHA-256's length.
    const Hmac hmac(password, password_len);
    const uint8_t index[4] = {0, 0, 0, 1};
    uint8_t u[32];
    hmac.mac(salt, salt_len, index, 4, u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iterations; ++i) {
        hmac.mac(u, 32, nullptr, 0, u);
        for (int b = 0; b < 32; ++b) {
            out[b] ^= u[b];
        }
    }
    return true;
}
#endif

uint32_t pbkdf2_iterations(uint32_t target_ms)
{
    if (const uint32_t known = calibrated.load()) {
        return known;
    }
    // A short run measured, scaled to the target: the board's speed with its
    // SHA hardware is what counts, and it is kept with each hash, so a later
    // firmware may choose differently without losing PINs.
    constexpr uint32_t PROBE = 1000;
    const uint8_t salt[16] = {};
    uint8_t out[32];
    const int64_t t0 = micros();
    const bool ok = pbkdf2_sha256(reinterpret_cast<const uint8_t *>("0000"), 4, salt, sizeof(salt), PROBE, out);
    const int64_t us = std::max<int64_t>(1, micros() - t0);
    uint64_t n = ok ? uint64_t(PROBE) * target_ms * 1000 / uint64_t(us) : 20000;
    // At least 10 000 even on a fast machine; at most what a slow day allows.
    n = std::min<uint64_t>(std::max<uint64_t>(n, 10000), 600000);
    n = n / 1000 * 1000;
    calibrated = uint32_t(n);
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "PIN hash: %u iterations (1000 took %lld us)", unsigned(n), (long long)us);
#endif
    return uint32_t(n);
}

void pbkdf2_force_iterations(uint32_t iterations)
{
    calibrated = iterations;
}

bool pbkdf2_self_test()
{
    const int known = self_test.load();
    if (known >= 0) {
        return known == 1;
    }
    // PBKDF2-HMAC-SHA256("password", "salt", c, 32) for c = 1 and 2.
    static const uint8_t C1[32] = {0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c, 0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
                                   0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48, 0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b
                                  };
    static const uint8_t C2[32] = {0xae, 0x4d, 0x0c, 0x95, 0xaf, 0x6b, 0x46, 0xd3, 0x2d, 0x0a, 0xdf, 0xf9, 0x28, 0xf0, 0x6d, 0xd0,
                                   0x2a, 0x30, 0x3f, 0x8e, 0xf3, 0xc2, 0x51, 0xdf, 0xd6, 0xe2, 0xd8, 0x5a, 0x95, 0x47, 0x4c, 0x43
                                  };
    const auto *pw = reinterpret_cast<const uint8_t *>("password");
    const auto *salt = reinterpret_cast<const uint8_t *>("salt");
    uint8_t out[32];
    const bool ok = pbkdf2_sha256(pw, 8, salt, 4, 1, out) && !memcmp(out, C1, 32) &&
                    pbkdf2_sha256(pw, 8, salt, 4, 2, out) && !memcmp(out, C2, 32);
#ifdef ESP_PLATFORM
    if (!ok) {
        ESP_LOGE(TAG, "the PIN hash failed its self-test: PINs are refused");
    }
#endif
    self_test = ok ? 1 : 0;
    return ok;
}

}  // namespace profiles_platform
