// SPDX-License-Identifier: Apache-2.0
// What profiles.cpp needs from the platform, so the model runs in host tests
// too: storage (NVS on the board, memory in tests), a clock, random bytes and
// PBKDF2-HMAC-SHA256 (PSA on the board, a portable copy on the host).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace profiles_platform {

// Blobs by key, in the "profiles" NVS namespace on the board.
struct Store {
    bool (*get)(const char *key, void *data, size_t *size);  // size: in the buffer's, out the blob's
    bool (*set)(const char *key, const void *data, size_t size);
    bool (*erase)(const char *key);  // true when gone (or never there)
};
const Store &nvs_store();

// Monotonic milliseconds and random bytes.
uint32_t now_ms();
void random_bytes(uint8_t *out, size_t n);

// PBKDF2-HMAC-SHA256 with a 32-byte result; false when it could not run.
bool pbkdf2_sha256(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[32]);
// Iterations that take about target_ms here (measured once, then kept).
uint32_t pbkdf2_iterations(uint32_t target_ms);
// Tests: use this many instead of measuring.
void pbkdf2_force_iterations(uint32_t iterations);
// The known answers of RFC 6070's SHA-256 counterparts, once: false when the
// hash is broken (then no PIN is stored or accepted).
bool pbkdf2_self_test();

}  // namespace profiles_platform

// For host tests: another store and clock (nullptr: the platform's), and the
// state forgotten as if the board restarted.
void profiles_test_platform(const profiles_platform::Store *store, uint32_t (*now_ms)());
void profiles_test_restart();
