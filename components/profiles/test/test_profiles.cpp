// SPDX-License-Identifier: Apache-2.0
// Host tests for the profiles model: first run, PIN hashing and checks, the
// growing wait, sign-in with face and PIN, full mode, the managers' rules,
// auto-lock and storage. Build and run:
//   cmake -S components/profiles/test -B build-host/profiles-unit && cmake --build build-host/profiles-unit
//   build-host/profiles-unit/profiles-test (host/render.py builds build-host/profiles-test too)
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "profiles.h"
#include "profiles_internal.h"

namespace {
int failures = 0, checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

// ---- a store in memory and a clock we move ----------------------------------------------
std::map<std::string, std::vector<uint8_t>> blobs;
bool get(const char *key, void *data, size_t *size)
{
    auto it = blobs.find(key);
    if (it == blobs.end() || it->second.size() > *size) {
        return false;
    }
    memcpy(data, it->second.data(), it->second.size());
    *size = it->second.size();
    return true;
}
bool set(const char *key, const void *data, size_t size)
{
    const auto *p = static_cast<const uint8_t *>(data);
    blobs[key].assign(p, p + size);
    return true;
}
bool erase(const char *key)
{
    blobs.erase(key);
    return true;
}
const profiles_platform::Store STORE = {get, set, erase};

uint32_t clock_ms = 1000;
uint32_t test_now()
{
    return clock_ms;
}

bool faces[16];
bool enrolled(uint8_t id)
{
    return id < 16 && faces[id];
}
std::vector<int> forgotten;
void forget(uint8_t id)
{
    forgotten.push_back(id);
    faces[id] = false;
}

int changes = 0;
void changed(void *)
{
    ++changes;
}

void fresh_board()
{
    blobs.clear();
    clock_ms = 1000;
    memset(faces, 0, sizeof(faces));
    forgotten.clear();
    profiles_test_restart();
    profiles_init();
}

void restart()
{
    profiles_test_restart();
    profiles_init();
}

profile_pin_t pin_of(const char *digits)
{
    profile_pin_t p{};
    CHECK(profiles_hash_pin(digits, &p));
    return p;
}

// Owner (id 1) with PIN 2468, signed in.
void set_up_owner()
{
    const profile_pin_t pin = pin_of("2468");
    uint8_t id = 0;
    CHECK_EQ(profiles_create_owner("Owner", &pin, &id), PROFILE_OK);
    CHECK_EQ(id, 1);
}

// ---- tests ---------------------------------------------------------------------------------
void pbkdf2_known_answers()
{
    using profiles_platform::pbkdf2_sha256;
    const auto *pw = reinterpret_cast<const uint8_t *>("password");
    const auto *salt = reinterpret_cast<const uint8_t *>("salt");
    uint8_t out[32];
    CHECK(pbkdf2_sha256(pw, 8, salt, 4, 4096, out));
    static const uint8_t C4096[32] = {0xc5, 0xe4, 0x78, 0xd5, 0x92, 0x88, 0xc8, 0x41, 0xaa, 0x53, 0x0d, 0xb6, 0x84, 0x5c, 0x4c, 0x8d,
                                      0x96, 0x28, 0x93, 0xa0, 0x01, 0xce, 0x4e, 0x11, 0xa4, 0x96, 0x38, 0x73, 0xaa, 0x98, 0x13, 0x4a
                                     };
    CHECK(!memcmp(out, C4096, 32));
    CHECK(profiles_platform::pbkdf2_self_test());
    // A key longer than SHA-256's block is hashed first (RFC 2104).
    const char *long_pw = "passwordPASSWORDpasswordPASSWORDpasswordPASSWORDpasswordPASSWORDxx";
    CHECK(pbkdf2_sha256(reinterpret_cast<const uint8_t *>(long_pw), strlen(long_pw), salt, 4, 1, out));
}

void pin_hashing()
{
    profile_pin_t a{}, b{};
    CHECK(!profiles_hash_pin(nullptr, &a));
    CHECK(!profiles_hash_pin("123", &a));
    CHECK(!profiles_hash_pin("1234567", &a));
    CHECK(!profiles_hash_pin("12a4", &a));
    CHECK(!profiles_hash_pin("12 34", &a));
    CHECK(profiles_hash_pin("1234", &a));
    CHECK(profiles_hash_pin("1234", &b));
    CHECK(a.iterations >= 10000);
    CHECK(memcmp(a.salt, b.salt, 16) != 0);  // a new salt each time
    CHECK(memcmp(a.hash, b.hash, 32) != 0);
    CHECK(profiles_hash_pin("123456", &a));
}

void first_run_is_as_before()
{
    fresh_board();
    CHECK(!profiles_configured());
    CHECK_EQ(profiles_current(), PROFILE_IMPLICIT);
    for (int c = 0; c < PROFILE_CAP_COUNT; ++c) {
        CHECK_EQ(profiles_can(profile_cap_t(c)), c != PROFILE_CAP_SWIFTY_ADVANCED);
    }
    for (int app = 0; app < 40; ++app) {
        CHECK_EQ(profiles_app_access(app), PROFILE_ACCESS_FULL);
    }
    CHECK(!profiles_elevated());
    CHECK(!profiles_idle(10u * 3600 * 1000));  // nothing locks
    CHECK_EQ(profiles_check_pin(1, "1234"), PROFILE_ERR_NO_PROFILE);
    profile_t p{};
    profiles_defaults(PROFILE_NEW, &p);
    CHECK_EQ(profiles_save(&p, PROFILE_BY_SESSION), PROFILE_ERR_DENIED);  // set up an owner first
    CHECK(blobs.empty());  // nothing written until profiles are set up
}

void owner_and_guest()
{
    fresh_board();
    set_up_owner();
    CHECK(profiles_configured());
    CHECK_EQ(profiles_current(), 1);
    CHECK(profiles_can(PROFILE_CAP_SETTINGS_PROFILES));
    CHECK(profiles_can(PROFILE_CAP_HOME_CONTROL));
    CHECK(!profiles_can(PROFILE_CAP_SWIFTY_ADVANCED));  // ticked, but only in full mode
    CHECK(!profiles_can(PROFILE_CAP_MACHINE_ACTIONS));
    CHECK_EQ(profiles_app_access(1), PROFILE_ACCESS_FULL);
    // A second set-up is refused.
    const profile_pin_t pin = pin_of("1111");
    CHECK_EQ(profiles_create_owner("Again", &pin, nullptr), PROFILE_ERR_DENIED);
    // A restart opens on Guest.
    restart();
    CHECK(profiles_configured());
    CHECK_EQ(profiles_current(), PROFILE_GUEST);
    CHECK_EQ(profiles_app_access(2), PROFILE_ACCESS_READ);    // Home
    CHECK_EQ(profiles_app_access(1), PROFILE_ACCESS_HIDDEN);  // Glimdock
    CHECK_EQ(profiles_app_access(10), PROFILE_ACCESS_FULL);   // Calculator
    CHECK_EQ(profiles_app_access(0), PROFILE_ACCESS_FULL);    // the launcher, always
    CHECK_EQ(profiles_app_access(PROFILE_APP_SETTINGS), PROFILE_ACCESS_HIDDEN);
    CHECK_EQ(profiles_app_access(99), PROFILE_ACCESS_HIDDEN);
    for (int c = 0; c < PROFILE_CAP_COUNT; ++c) {
        CHECK(!profiles_can(profile_cap_t(c)));
    }
    profile_t list[PROFILES_MAX + 1];
    CHECK_EQ(profiles_list(list, PROFILES_MAX + 1), 2);
    CHECK_EQ(list[0].id, PROFILE_GUEST);
    CHECK(!strcmp(list[1].name, "Owner"));
    CHECK(list[1].has_pin);
    // No PIN digits anywhere in the store.
    for (const auto &kv : blobs) {
        const std::string s(kv.second.begin(), kv.second.end());
        CHECK(s.find("2468") == std::string::npos);
    }
}

void pin_tries_and_waits()
{
    fresh_board();
    set_up_owner();
    restart();  // Guest
    CHECK_EQ(profiles_sign_in(1), PROFILE_ERR_NEED_PIN);
    CHECK_EQ(profiles_tries_left(1), 4);
    CHECK_EQ(profiles_check_pin(1, "1111"), PROFILE_ERR_WRONG_PIN);
    CHECK_EQ(profiles_check_pin(1, "2222"), PROFILE_ERR_WRONG_PIN);
    CHECK_EQ(profiles_check_pin(1, "3333"), PROFILE_ERR_WRONG_PIN);
    CHECK_EQ(profiles_tries_left(1), 1);
    CHECK_EQ(profiles_check_pin(1, "4444"), PROFILE_ERR_WAIT);
    CHECK_EQ(profiles_wait_s(1), 30u);
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_ERR_WAIT);  // even the right one, meanwhile
    CHECK_EQ(profiles_check_pin(1, "12"), PROFILE_ERR_INVALID);  // not a PIN: not counted
    // A restart does not skip the wait: it starts again.
    clock_ms += 20000;
    restart();
    CHECK_EQ(profiles_wait_s(1), 30u);
    clock_ms += 30000;
    CHECK_EQ(profiles_wait_s(1), 0u);
    // Each further wrong try doubles it.
    CHECK_EQ(profiles_check_pin(1, "5555"), PROFILE_ERR_WAIT);
    CHECK_EQ(profiles_wait_s(1), 60u);
    clock_ms += 60000;
    CHECK_EQ(profiles_check_pin(1, "6666"), PROFILE_ERR_WAIT);
    CHECK_EQ(profiles_wait_s(1), 120u);
    clock_ms += 120000;
    // The right PIN, then sign in: the count is forgotten.
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_OK);
    CHECK_EQ(profiles_sign_in(1), PROFILE_OK);
    CHECK_EQ(profiles_current(), 1);
    CHECK_EQ(profiles_tries_left(1), 4);
    CHECK_EQ(profiles_wait_s(1), 0u);
    // The cap: half an hour.
    for (int i = 0; i < 20; ++i) {
        clock_ms += 3600 * 1000;
        profiles_check_pin(1, "0000");
    }
    CHECK_EQ(profiles_wait_s(1), 1800u);
    CHECK_EQ(profiles_unblock(1, PROFILE_BY_TRUSTED), PROFILE_OK);
    CHECK_EQ(profiles_wait_s(1), 0u);
}

void tickets_expire()
{
    fresh_board();
    set_up_owner();
    profiles_lock();
    CHECK_EQ(profiles_current(), PROFILE_GUEST);
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_OK);
    clock_ms += 61000;
    CHECK_EQ(profiles_sign_in(1), PROFILE_ERR_NEED_PIN);  // a minute old: ask again
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_OK);
    CHECK_EQ(profiles_sign_in(1), PROFILE_OK);
    profiles_lock();
    CHECK_EQ(profiles_sign_in(1), PROFILE_ERR_NEED_PIN);  // used up by the last sign-in
    // A PIN checked for one profile does not open another.
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_OK);
    CHECK_EQ(profiles_sign_in(2), PROFILE_ERR_NO_PROFILE);
}

void partner_without_glimdock()
{
    fresh_board();
    set_up_owner();
    profile_t p{};
    profiles_defaults(PROFILE_NEW, &p);
    p.id = PROFILE_NEW;
    strcpy(p.name, "  Partner ");
    CHECK_EQ(profiles_save(&p, PROFILE_BY_SESSION), PROFILE_OK);
    CHECK_EQ(p.id, 2);
    profile_t back{};
    CHECK(profiles_get(2, &back));
    CHECK(!strcmp(back.name, "Partner"));  // trimmed
    // No sign-in: a tap opens it.
    CHECK_EQ(profiles_sign_in(2), PROFILE_OK);
    CHECK_EQ(profiles_app_access(1), PROFILE_ACCESS_HIDDEN);  // no Glimdock
    CHECK_EQ(profiles_app_access(2), PROFILE_ACCESS_FULL);    // Home
    CHECK(profiles_can(PROFILE_CAP_HOME_CONTROL));
    CHECK(!profiles_can(PROFILE_CAP_SETTINGS_PROFILES));
    CHECK(!profiles_can(PROFILE_CAP_GLIMDOCK_ACTIONS));
    // Partner may not edit profiles...
    back.apps[1] = PROFILE_ACCESS_FULL;
    CHECK_EQ(profiles_save(&back, PROFILE_BY_SESSION), PROFILE_ERR_DENIED);
    CHECK_EQ(profiles_delete(1, PROFILE_BY_SESSION), PROFILE_ERR_DENIED);
    CHECK_EQ(profiles_turn_off(PROFILE_BY_SESSION), PROFILE_ERR_DENIED);
    // ...but may set its own PIN, and then it asks for it.
    const profile_pin_t pin = pin_of("1357");
    CHECK_EQ(profiles_set_pin(2, &pin, PROFILE_BY_SESSION), PROFILE_OK);
    CHECK_EQ(profiles_set_pin(1, &pin, PROFILE_BY_SESSION), PROFILE_ERR_DENIED);
    CHECK(profiles_get(2, &back));
    CHECK_EQ(back.signin, PROFILE_SIGNIN_PIN);
    profiles_lock();
    CHECK_EQ(profiles_sign_in(2), PROFILE_ERR_NEED_PIN);
    // Bad names and colours.
    profile_t bad = back;
    strcpy(bad.name, "   ");
    CHECK_EQ(profiles_save(&bad, PROFILE_BY_TRUSTED), PROFILE_ERR_INVALID);
    strcpy(bad.name, "bad\nname");
    CHECK_EQ(profiles_save(&bad, PROFILE_BY_TRUSTED), PROFILE_ERR_INVALID);
    memset(bad.name, 'x', sizeof(bad.name) - 1);
    bad.name[sizeof(bad.name) - 1] = 0;
    CHECK_EQ(profiles_save(&bad, PROFILE_BY_TRUSTED), PROFILE_OK);  // 19 bytes: the longest
}

void managers_need_a_pin_and_one_stays()
{
    fresh_board();
    set_up_owner();
    profile_t p{};
    profiles_defaults(PROFILE_NEW, &p);
    p.id = PROFILE_NEW;
    strcpy(p.name, "Partner");
    p.caps |= PROFILE_CAP(PROFILE_CAP_SETTINGS_PROFILES);
    CHECK_EQ(profiles_save(&p, PROFILE_BY_SESSION), PROFILE_ERR_INVALID);  // no PIN to sign in with
    p.caps &= ~PROFILE_CAP(PROFILE_CAP_SETTINGS_PROFILES);
    p.signin = PROFILE_SIGNIN_PIN;
    CHECK_EQ(profiles_save(&p, PROFILE_BY_SESSION), PROFILE_ERR_NEED_PIN);
    p.signin = PROFILE_SIGNIN_NONE;
    CHECK_EQ(profiles_save(&p, PROFILE_BY_SESSION), PROFILE_OK);
    // The only manager stays.
    CHECK_EQ(profiles_delete(1, PROFILE_BY_SESSION), PROFILE_ERR_LAST_MANAGER);
    CHECK_EQ(profiles_delete(1, PROFILE_BY_TRUSTED), PROFILE_ERR_LAST_MANAGER);
    profile_t owner{};
    CHECK(profiles_get(1, &owner));
    owner.caps &= ~PROFILE_CAP(PROFILE_CAP_SETTINGS_PROFILES);
    CHECK_EQ(profiles_save(&owner, PROFILE_BY_SESSION), PROFILE_ERR_LAST_MANAGER);
    owner.caps |= PROFILE_CAP(PROFILE_CAP_SETTINGS_PROFILES);
    owner.signin = PROFILE_SIGNIN_NONE;
    CHECK_EQ(profiles_save(&owner, PROFILE_BY_SESSION), PROFILE_ERR_INVALID);
    // Guest is never a manager and never deleted.
    profile_t guest{};
    CHECK(profiles_get(PROFILE_GUEST, &guest));
    guest.caps = PROFILE_CAPS_ALL;
    guest.signin = PROFILE_SIGNIN_PIN;
    guest.elevated = true;
    CHECK_EQ(profiles_save(&guest, PROFILE_BY_SESSION), PROFILE_OK);
    CHECK(profiles_get(PROFILE_GUEST, &guest));
    CHECK_EQ(guest.signin, PROFILE_SIGNIN_NONE);
    CHECK(!guest.elevated);
    CHECK(!(guest.caps & (PROFILE_CAP(PROFILE_CAP_SETTINGS_PROFILES) | PROFILE_CAPS_ELEVATED)));
    CHECK_EQ(profiles_delete(PROFILE_GUEST, PROFILE_BY_TRUSTED), PROFILE_ERR_INVALID);
    // Deleting Partner forgets its face.
    faces[2] = true;
    CHECK_EQ(profiles_delete(2, PROFILE_BY_SESSION), PROFILE_OK);
    CHECK_EQ(forgotten.size(), 1u);
    CHECK(!faces[2]);
    // Eight at most.
    for (int i = 0; i < PROFILES_MAX - 1; ++i) {
        profile_t n{};
        profiles_defaults(PROFILE_NEW, &n);
        n.id = PROFILE_NEW;
        snprintf(n.name, sizeof(n.name), "P%d", i);
        CHECK_EQ(profiles_save(&n, PROFILE_BY_SESSION), PROFILE_OK);
    }
    profile_t extra{};
    profiles_defaults(PROFILE_NEW, &extra);
    extra.id = PROFILE_NEW;
    strcpy(extra.name, "One too many");
    CHECK_EQ(profiles_save(&extra, PROFILE_BY_SESSION), PROFILE_ERR_FULL);
}

void full_mode_needs_face_and_pin()
{
    fresh_board();
    profiles_on_change(changed, nullptr);
    set_up_owner();
    profile_t owner{};
    CHECK(profiles_get(1, &owner));
    owner.elevated = true;
    CHECK_EQ(profiles_save(&owner, PROFILE_BY_SESSION), PROFILE_ERR_INVALID);  // still PIN only
    owner.signin = PROFILE_SIGNIN_FACE_PIN;
    CHECK_EQ(profiles_save(&owner, PROFILE_BY_SESSION), PROFILE_ERR_NEED_FACE);
    faces[1] = true;  // enrolled (face_id)
    CHECK_EQ(profiles_save(&owner, PROFILE_BY_SESSION), PROFILE_OK);
    CHECK(profiles_any_elevated());
    // The PIN-only session that made it no longer manages: full mode does now.
    CHECK(!profiles_elevated());
    CHECK(!profiles_can(PROFILE_CAP_SETTINGS_PROFILES));
    // Signing in takes both, within a minute.
    profiles_lock();
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_OK);
    CHECK_EQ(profiles_sign_in(1), PROFILE_ERR_NEED_FACE);
    profiles_face_matched(2);  // someone else's face
    CHECK_EQ(profiles_sign_in(1), PROFILE_ERR_NEED_FACE);
    profiles_face_matched(1);
    const int before = changes;
    CHECK_EQ(profiles_sign_in(1), PROFILE_OK);
    CHECK(changes > before);
    CHECK(profiles_elevated());
    CHECK(profiles_can(PROFILE_CAP_SWIFTY_ADVANCED));
    CHECK(profiles_can(PROFILE_CAP_MACHINE_ACTIONS));
    CHECK(profiles_can(PROFILE_CAP_PASEO));
    CHECK(profiles_can(PROFILE_CAP_GLIMDOCK_ACTIONS));
    CHECK(profiles_can(PROFILE_CAP_SETTINGS_PROFILES));
    // The face alone is not enough either.
    profiles_lock();
    profiles_face_matched(1);
    CHECK_EQ(profiles_sign_in(1), PROFILE_ERR_NEED_PIN);
    // Full mode locks sooner (two minutes by default).
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_OK);
    CHECK_EQ(profiles_sign_in(1), PROFILE_OK);
    CHECK_EQ(profiles_lock_after_s(), 120u);
    clock_ms += 200000;
    CHECK(!profiles_idle(119000));
    CHECK(profiles_idle(120000));
    CHECK_EQ(profiles_current(), PROFILE_GUEST);
    CHECK(!profiles_elevated());
    // Faces that do not match count as wrong tries.
    for (int i = 0; i < 4; ++i) {
        profiles_face_failed(1);
    }
    CHECK_EQ(profiles_wait_s(1), 30u);
}

void non_elevated_cannot_escalate()
{
    fresh_board();
    set_up_owner();
    faces[1] = true;
    profile_t owner{};
    CHECK(profiles_get(1, &owner));
    owner.signin = PROFILE_SIGNIN_FACE_PIN;
    owner.elevated = true;
    CHECK_EQ(profiles_save(&owner, PROFILE_BY_SESSION), PROFILE_OK);
    // Partner with a PIN and the manage tick, made over USB (trusted).
    profile_t p{};
    profiles_defaults(PROFILE_NEW, &p);
    p.id = PROFILE_NEW;
    strcpy(p.name, "Partner");
    CHECK_EQ(profiles_save(&p, PROFILE_BY_TRUSTED), PROFILE_OK);
    const profile_pin_t pin = pin_of("8642");
    CHECK_EQ(profiles_set_pin(p.id, &pin, PROFILE_BY_TRUSTED), PROFILE_OK);
    CHECK(profiles_get(p.id, &p));
    p.caps |= PROFILE_CAP(PROFILE_CAP_SETTINGS_PROFILES);
    CHECK_EQ(profiles_save(&p, PROFILE_BY_TRUSTED), PROFILE_OK);
    profiles_lock();
    CHECK_EQ(profiles_check_pin(p.id, "8642"), PROFILE_OK);
    CHECK_EQ(profiles_sign_in(p.id), PROFILE_OK);
    // Ticked, but a profile has full mode: only full mode manages.
    CHECK(!profiles_can(PROFILE_CAP_SETTINGS_PROFILES));
    faces[p.id] = true;
    p.signin = PROFILE_SIGNIN_FACE_PIN;
    p.elevated = true;
    CHECK_EQ(profiles_save(&p, PROFILE_BY_SESSION), PROFILE_ERR_DENIED);
    CHECK_EQ(profiles_delete(1, PROFILE_BY_SESSION), PROFILE_ERR_DENIED);
}

void auto_lock_and_default()
{
    fresh_board();
    set_up_owner();
    CHECK_EQ(profiles_lock_after_s(), 300u);
    clock_ms += 1000000;
    CHECK(!profiles_idle(299000));
    CHECK(profiles_idle(300000));
    CHECK_EQ(profiles_current(), PROFILE_GUEST);
    CHECK(!profiles_idle(10u * 3600 * 1000));  // Guest does not lock
    // Signed in just now (over USB, say): the idle time before it does not count.
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_OK);
    CHECK_EQ(profiles_sign_in(1), PROFILE_OK);
    CHECK(!profiles_idle(10u * 3600 * 1000));
    // A family profile without sign-in as what the board locks to.
    profile_t p{};
    profiles_defaults(PROFILE_NEW, &p);
    p.id = PROFILE_NEW;
    strcpy(p.name, "Family");
    CHECK_EQ(profiles_save(&p, PROFILE_BY_SESSION), PROFILE_OK);
    profiles_settings_t s{};
    profiles_get_settings(&s);
    s.default_id = p.id;
    s.lock_s = 0;
    CHECK_EQ(profiles_set_settings(&s, PROFILE_BY_SESSION), PROFILE_OK);
    CHECK_EQ(profiles_lock_after_s(), 0u);  // never, for the owner now
    s.elevated_lock_s = 0;
    CHECK_EQ(profiles_set_settings(&s, PROFILE_BY_SESSION), PROFILE_ERR_INVALID);  // full mode always locks
    s.elevated_lock_s = 120;
    s.default_id = 1;
    CHECK_EQ(profiles_set_settings(&s, PROFILE_BY_SESSION), PROFILE_ERR_INVALID);  // the owner needs a PIN
    profiles_lock();
    CHECK_EQ(profiles_current(), p.id);
    restart();
    CHECK_EQ(profiles_current(), p.id);  // and starts on it
    // Family gets a PIN: no longer something to lock to; Guest again.
    const profile_pin_t pin = pin_of("9999");
    CHECK_EQ(profiles_set_pin(p.id, &pin, PROFILE_BY_TRUSTED), PROFILE_OK);
    profiles_get_settings(&s);
    CHECK_EQ(s.default_id, PROFILE_GUEST);
}

void turning_off_restores_everything()
{
    fresh_board();
    set_up_owner();
    faces[1] = true;
    restart();
    CHECK_EQ(profiles_turn_off(PROFILE_BY_SESSION), PROFILE_ERR_DENIED);  // Guest may not
    CHECK_EQ(profiles_turn_off(PROFILE_BY_TRUSTED), PROFILE_OK);
    CHECK(!profiles_configured());
    CHECK(profiles_can(PROFILE_CAP_SETTINGS_PROFILES));
    CHECK_EQ(profiles_app_access(1), PROFILE_ACCESS_FULL);
    CHECK(blobs.empty());
    CHECK(!faces[1]);
    restart();
    CHECK(!profiles_configured());
}

void storage_is_checked()
{
    fresh_board();
    set_up_owner();
    // A record of the wrong size or version is ignored, never half-read.
    std::vector<uint8_t> owner = blobs["p1"];
    blobs["p1"].resize(owner.size() - 1);
    restart();
    CHECK(!profiles_configured());  // Guest alone is not a set-up board
    blobs["p1"] = owner;
    blobs["p1"][0] = 99;  // a later layout
    restart();
    CHECK(!profiles_configured());
    blobs["p1"] = owner;
    blobs["p1"][1] = 3;  // stored under the wrong id
    restart();
    CHECK(!profiles_configured());
    blobs["p1"] = owner;
    blobs.erase("p0");  // Guest lost: made again
    restart();
    CHECK(profiles_configured());
    CHECK_EQ(profiles_current(), PROFILE_GUEST);
    CHECK(blobs.count("p0"));
    CHECK_EQ(profiles_check_pin(1, "2468"), PROFILE_OK);
}

void listeners_hear_changes()
{
    fresh_board();
    changes = 0;
    set_up_owner();
    const int after_setup = changes;
    CHECK(after_setup >= 1);
    profiles_lock();
    CHECK(changes > after_setup);
    const int after_lock = changes;
    profiles_lock();  // already locked
    CHECK_EQ(changes, after_lock);
}
}  // namespace

int main()
{
    profiles_test_platform(&STORE, test_now);
    profiles_platform::pbkdf2_force_iterations(10000);  // the board's floor; the board measures its own
    profiles_set_face_hooks(enrolled, forget);
    pbkdf2_known_answers();
    pin_hashing();
    first_run_is_as_before();
    owner_and_guest();
    pin_tries_and_waits();
    tickets_expire();
    partner_without_glimdock();
    managers_need_a_pin_and_one_stays();
    full_mode_needs_face_and_pin();
    non_elevated_cannot_escalate();
    auto_lock_and_default();
    turning_off_restores_everything();
    storage_is_checked();
    listeners_hear_changes();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
