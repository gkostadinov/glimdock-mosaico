// SPDX-License-Identifier: Apache-2.0
// The profiles model (profiles.h). Plain C++ over profiles_internal.h's
// platform, so the host tests run the same code as the board.
//
// NVS ("profiles"), each blob led by its layout's version:
//   p0 .. p8  a profile (Record): its choices and its PIN's salt and hash
//   cfg       the owner's settings (Config)
//   fails     wrong tries per profile (one byte each), kept across restarts
//             so a restart does not reset the wait
#include "profiles.h"
#include "profiles_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace {
using namespace profiles_platform;

constexpr int SLOTS = PROFILES_MAX + 1;
constexpr uint32_t TICKET_MS = 60000;     // a matched face or a right PIN counts for a minute
constexpr int FREE_TRIES = 4;             // wrong tries before the first wait
constexpr uint32_t FIRST_WAIT_S = 30;     // then 30 s, doubling with every further wrong try
constexpr uint32_t MAX_WAIT_S = 30 * 60;  // up to half an hour
constexpr uint32_t PIN_TARGET_MS = 400;   // the hash's cost on this board
constexpr uint8_t RECORD_VERSION = 1, CONFIG_VERSION = 1;
constexpr uint32_t MANAGE = PROFILE_CAP(PROFILE_CAP_SETTINGS_PROFILES);

struct Record {
    uint8_t version, id, color, signin, flags, reserved[3];
    char name[PROFILE_NAME_LEN];
    uint32_t caps;
    uint8_t apps[PROFILE_APPS];
    uint32_t iterations;
    uint8_t salt[16], hash[32];
};
static_assert(sizeof(Record) == 116, "the stored layout of version 1");
constexpr uint8_t FLAG_ELEVATED = 1, FLAG_PIN = 2;

struct Config {
    uint8_t version, default_id, reserved[2];
    uint16_t lock_s, elevated_lock_s;
};
static_assert(sizeof(Config) == 8, "the stored layout of version 1");

struct Slot {
    bool used = false;
    profile_t p{};
    profile_pin_t pin{};
};

struct Ticket {
    uint8_t id = 0xFF;
    uint32_t at = 0;
};

struct State {
    bool loaded = false, configured = false;
    Slot slots[SLOTS];
    profiles_settings_t settings{PROFILE_GUEST, 300, 120};
    uint8_t fails[SLOTS] = {};
    uint32_t wait_until[SLOTS] = {};  // now_ms() when a wait ends; 0 none
    uint8_t current = PROFILE_IMPLICIT;
    // Signed in with face and PIN as an elevated profile. Not the profile's
    // flag alone: turning full mode on for the profile in use does not make
    // this session elevated until it signs in again.
    bool session_elevated = false;
    uint32_t signed_in_at = 0;
    Ticket pin, face;
};

std::mutex lock;
State S;
struct Listener {
    void (*fn)(void *);
    void *user;
};
Listener listeners[8];
std::atomic<int> listener_count{0};
bool (*face_enrolled_hook)(uint8_t) = nullptr;
void (*face_forget_hook)(uint8_t) = nullptr;
const Store *store_override = nullptr;
uint32_t (*clock_override)() = nullptr;

// What every reader sees (profiles_can and friends): set by publish().
constexpr uint32_t IMPLICIT_CAPS = PROFILE_CAPS_ALL & ~PROFILE_CAP(PROFILE_CAP_SWIFTY_ADVANCED);
std::atomic<uint8_t> snap_id{PROFILE_IMPLICIT};
std::atomic<uint32_t> snap_caps{IMPLICIT_CAPS}, snap_shown{~0u}, snap_full{~0u};
std::atomic<bool> snap_configured{false}, snap_elevated{false}, snap_any_elevated{false};

const Store &store()
{
    return store_override ? *store_override : nvs_store();
}

uint32_t now()
{
    return clock_override ? clock_override() : now_ms();
}

bool before(uint32_t a, uint32_t b)  // a is earlier than b, across the 49-day wrap
{
    return int32_t(a - b) < 0;
}

bool fresh(const Ticket &t, uint8_t id)
{
    return t.id == id && uint32_t(now() - t.at) < TICKET_MS;
}

bool wants_pin(uint8_t signin)
{
    return signin == PROFILE_SIGNIN_PIN || signin == PROFILE_SIGNIN_FACE_PIN;
}

bool wants_face(uint8_t signin)
{
    return signin == PROFILE_SIGNIN_FACE || signin == PROFILE_SIGNIN_FACE_PIN;
}

bool face_enrolled(uint8_t id)
{
    return face_enrolled_hook && face_enrolled_hook(id);
}

void key_for(char out[8], uint8_t id)
{
    snprintf(out, 8, "p%u", unsigned(id));
}

// ---- defaults ------------------------------------------------------------------------------
// App slots are ShellApp values (shell_internal.h; stored, so stable):
// 0 launcher, 1 Glimdock, 2 Home, 3 Camera, 4 Swifty, 5 Files, 6 Tools,
// 7 Gallery, 8 Webcam, 9 Interaction, 10 Calculator, 11 VPN, 12 Games.
enum : uint8_t { A_LAUNCHER = 0, A_GLIMDOCK = 1, A_HOME = 2, A_CAMERA = 3, A_SWIFTY = 4, A_FILES = 5, A_TOOLS = 6,
                 A_GALLERY = 7, A_WEBCAM = 8, A_INTERACTION = 9, A_CALCULATOR = 10, A_VPN = 11, A_GAMES = 12
               };

void fill_defaults(uint8_t id, profile_t &p)
{
    memset(&p, 0, sizeof(p));
    p.id = id;
    p.signin = PROFILE_SIGNIN_NONE;
    if (id == PROFILE_GUEST) {
        // A visitor: look at the home and ask Swifty, a few harmless tools.
        // Nothing of the homelab, the files or the photos. The owner changes it.
        strlcpy(p.name, "Guest", sizeof(p.name));
        p.color = PROFILE_COLORS - 1;
        p.caps = 0;
        p.apps[A_HOME] = p.apps[A_SWIFTY] = PROFILE_ACCESS_READ;
        p.apps[A_TOOLS] = p.apps[A_CALCULATOR] = p.apps[A_GAMES] = PROFILE_ACCESS_FULL;
    } else {
        // Someone at home: the home and everyday apps, not Glimdock, the NAS
        // or the VPN. Apps a later firmware adds start shown.
        strlcpy(p.name, "New profile", sizeof(p.name));
        p.color = uint8_t(id % (PROFILE_COLORS - 1));
        p.caps = PROFILE_CAP(PROFILE_CAP_HOME_CONTROL) | PROFILE_CAP(PROFILE_CAP_BOARD_CONTROL) |
                 PROFILE_CAP(PROFILE_CAP_WEB_SEARCH) | PROFILE_CAP(PROFILE_CAP_SHORTCUTS_RUN);
        memset(p.apps, PROFILE_ACCESS_FULL, sizeof(p.apps));
        p.apps[A_GLIMDOCK] = p.apps[A_FILES] = p.apps[A_VPN] = PROFILE_ACCESS_HIDDEN;
    }
    p.apps[A_LAUNCHER] = PROFILE_ACCESS_FULL;
}

// ---- storage -------------------------------------------------------------------------------
bool save_slot(uint8_t id)
{
    const Slot &s = S.slots[id];
    Record r{};
    r.version = RECORD_VERSION;
    r.id = id;
    r.color = s.p.color;
    r.signin = s.p.signin;
    r.flags = uint8_t((s.p.elevated ? FLAG_ELEVATED : 0) | (s.p.has_pin ? FLAG_PIN : 0));
    memcpy(r.name, s.p.name, sizeof(r.name));
    r.caps = s.p.caps;
    memcpy(r.apps, s.p.apps, sizeof(r.apps));
    if (s.p.has_pin) {
        r.iterations = s.pin.iterations;
        memcpy(r.salt, s.pin.salt, sizeof(r.salt));
        memcpy(r.hash, s.pin.hash, sizeof(r.hash));
    }
    char key[8];
    key_for(key, id);
    const bool ok = store().set(key, &r, sizeof(r));
    memset(&r, 0, sizeof(r));
    return ok;
}

bool load_slot(uint8_t id, Slot &s)
{
    Record r{};
    size_t size = sizeof(r);
    char key[8];
    key_for(key, id);
    if (!store().get(key, &r, &size) || size != sizeof(r) || r.version != RECORD_VERSION || r.id != id) {
        return false;
    }
    s.used = true;
    s.p.id = id;
    memcpy(s.p.name, r.name, sizeof(s.p.name));
    s.p.name[PROFILE_NAME_LEN - 1] = 0;
    s.p.color = uint8_t(r.color % PROFILE_COLORS);
    s.p.signin = std::min<uint8_t>(r.signin, PROFILE_SIGNIN_FACE_PIN);
    s.p.elevated = r.flags & FLAG_ELEVATED;
    s.p.has_pin = (r.flags & FLAG_PIN) && r.iterations;
    s.p.caps = r.caps & PROFILE_CAPS_ALL;
    for (int i = 0; i < PROFILE_APPS; ++i) {
        s.p.apps[i] = std::min<uint8_t>(r.apps[i], PROFILE_ACCESS_FULL);
    }
    s.p.apps[A_LAUNCHER] = PROFILE_ACCESS_FULL;
    s.pin.iterations = r.iterations;
    memcpy(s.pin.salt, r.salt, sizeof(s.pin.salt));
    memcpy(s.pin.hash, r.hash, sizeof(s.pin.hash));
    memset(&r, 0, sizeof(r));
    return true;
}

bool save_config()
{
    Config c{};
    c.version = CONFIG_VERSION;
    c.default_id = S.settings.default_id;
    c.lock_s = S.settings.lock_s;
    c.elevated_lock_s = S.settings.elevated_lock_s;
    return store().set("cfg", &c, sizeof(c));
}

bool save_fails()
{
    return store().set("fails", S.fails, sizeof(S.fails));
}

uint32_t wait_for(int fails)
{
    if (fails < FREE_TRIES) {
        return 0;
    }
    const int doublings = std::min(fails - FREE_TRIES, 10);
    return std::min<uint32_t>(FIRST_WAIT_S << doublings, MAX_WAIT_S);
}

// The default profile must open without sign-in; otherwise Guest.
void check_default()
{
    const uint8_t d = S.settings.default_id;
    if (d >= SLOTS || !S.slots[d].used || S.slots[d].p.signin != PROFILE_SIGNIN_NONE) {
        S.settings.default_id = PROFILE_GUEST;
    }
}

// ---- the snapshot ----------------------------------------------------------------------------
// May the session's own profile manage profiles: it needs the tick and a PIN.
bool can_manage(const profile_t &p)
{
    return (p.caps & MANAGE) && p.has_pin && wants_pin(p.signin);
}

bool any_elevated_locked()
{
    for (int i = 1; i < SLOTS; ++i)
        if (S.slots[i].used && S.slots[i].p.elevated) {
            return true;
        }
    return false;
}

void publish()
{
    if (!S.configured) {
        snap_id = PROFILE_IMPLICIT;
        snap_caps = IMPLICIT_CAPS;
        snap_shown = ~0u;
        snap_full = ~0u;
        snap_elevated = false;
        snap_any_elevated = false;
        snap_configured = false;
        return;
    }
    const profile_t &p = S.slots[S.current].p;
    const bool elevated = p.elevated && S.session_elevated && S.current != PROFILE_GUEST;
    const bool any = any_elevated_locked();
    uint32_t caps = p.caps & PROFILE_CAPS_ALL;
    if (!elevated) {
        caps &= ~PROFILE_CAPS_ELEVATED;
    }
    // Once a profile has full mode, only full mode manages profiles (else a
    // manager could make itself elevated with its own face).
    if (!can_manage(p) || (any && !elevated)) {
        caps &= ~MANAGE;
    }
    uint32_t shown = 0, full = 0;
    for (int i = 0; i < PROFILE_APPS; ++i) {
        if (p.apps[i] >= PROFILE_ACCESS_READ) {
            shown |= 1u << i;
        }
        if (p.apps[i] == PROFILE_ACCESS_FULL) {
            full |= 1u << i;
        }
    }
    snap_caps = caps;
    snap_shown = shown | 1u;
    snap_full = full | 1u;
    snap_elevated = elevated;
    snap_any_elevated = any;
    snap_id = S.current;
    snap_configured = true;
}

// Listeners run outside the lock (they read the snapshot and may call back in).
void notify()
{
    const int n = listener_count.load();
    for (int i = 0; i < n; ++i) {
        listeners[i].fn(listeners[i].user);
    }
}

void load_locked()
{
    if (S.loaded) {
        return;
    }
    S.loaded = true;
    S.configured = false;
    for (int i = 0; i < SLOTS; ++i) {
        S.slots[i] = Slot{};
        if (load_slot(uint8_t(i), S.slots[i]) && i > 0) {
            S.configured = true;
        }
    }
    Config c{};
    size_t size = sizeof(c);
    if (store().get("cfg", &c, &size) && size == sizeof(c) && c.version == CONFIG_VERSION) {
        S.settings.default_id = c.default_id;
        S.settings.lock_s = c.lock_s;
        S.settings.elevated_lock_s = c.elevated_lock_s ? c.elevated_lock_s : 120;
    }
    size = sizeof(S.fails);
    uint8_t fails[SLOTS] = {};
    if (store().get("fails", fails, &size) && size == sizeof(fails)) {
        memcpy(S.fails, fails, sizeof(S.fails));
    }
    if (S.configured && !S.slots[PROFILE_GUEST].used) {
        S.slots[PROFILE_GUEST].used = true;
        fill_defaults(PROFILE_GUEST, S.slots[PROFILE_GUEST].p);
        save_slot(PROFILE_GUEST);
    }
    check_default();
    // A wait that was running starts again in full: restarting does not skip it.
    const uint32_t t = now();
    for (int i = 0; i < SLOTS; ++i) {
        const uint32_t w = wait_for(S.fails[i]);
        S.wait_until[i] = w ? t + w * 1000 : 0;
    }
    S.current = S.configured ? S.settings.default_id : PROFILE_IMPLICIT;
    S.session_elevated = false;
    S.signed_in_at = t;
    publish();
}

// The tries left to wrong guesses: counted, saved, and a wait started.
void count_failure(uint8_t id)
{
    if (S.fails[id] < 255) {
        ++S.fails[id];
    }
    const uint32_t w = wait_for(S.fails[id]);
    S.wait_until[id] = w ? now() + w * 1000 : 0;
    save_fails();
}

void clear_failures(uint8_t id)
{
    S.wait_until[id] = 0;
    if (S.fails[id]) {
        S.fails[id] = 0;
        save_fails();
    }
}

bool waiting(uint8_t id)
{
    return S.wait_until[id] && before(now(), S.wait_until[id]);
}

bool valid_pin(const char *pin)
{
    const size_t n = pin ? strlen(pin) : 0;
    if (n < PROFILE_PIN_MIN || n > PROFILE_PIN_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; ++i)
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    return true;
}

// A name: trimmed, 1 to 19 bytes, no control characters.
bool clean_name(const char *in, char out[PROFILE_NAME_LEN])
{
    if (!in) {
        return false;
    }
    while (*in == ' ') {
        ++in;
    }
    size_t n = strlen(in);
    while (n && in[n - 1] == ' ') {
        --n;
    }
    if (!n || n >= PROFILE_NAME_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; ++i)
        if (uint8_t(in[i]) < 0x20 || in[i] == 0x7F) {
            return false;
        }
    memcpy(out, in, n);
    out[n] = 0;
    return true;
}

bool may_manage(profile_editor_t by)
{
    return by == PROFILE_BY_TRUSTED || !S.configured || (snap_caps.load() & MANAGE);
}

// After a change: one profile at least must still be able to manage the rest.
int managers(const Slot *slots)
{
    int n = 0;
    for (int i = 1; i < SLOTS; ++i)
        if (slots[i].used && can_manage(slots[i].p)) {
            ++n;
        }
    return n;
}

void reset_state()
{
    for (int i = 0; i < SLOTS; ++i) {
        S.slots[i] = Slot{};
        S.fails[i] = 0;
        S.wait_until[i] = 0;
    }
    S.settings = profiles_settings_t{PROFILE_GUEST, 300, 120};
    S.configured = false;
    S.current = PROFILE_IMPLICIT;
    S.session_elevated = false;
    S.pin = S.face = Ticket{};
}
}  // namespace

// ---- state -------------------------------------------------------------------------------
extern "C" void profiles_init(void)
{
    std::lock_guard<std::mutex> g(lock);
    load_locked();
}

extern "C" bool profiles_configured(void)
{
    return snap_configured;
}

extern "C" uint8_t profiles_current(void)
{
    return snap_id;
}

extern "C" bool profiles_get(uint8_t id, profile_t *out)
{
    if (!out) {
        return false;
    }
    if (id == PROFILE_IMPLICIT) {
        fill_defaults(1, *out);
        out->id = PROFILE_IMPLICIT;
        strlcpy(out->name, "Owner", sizeof(out->name));
        out->color = 0;
        out->caps = IMPLICIT_CAPS;
        memset(out->apps, PROFILE_ACCESS_FULL, sizeof(out->apps));
        return true;
    }
    std::lock_guard<std::mutex> g(lock);
    load_locked();
    if (id >= SLOTS || !S.slots[id].used) {
        return false;
    }
    *out = S.slots[id].p;
    return true;
}

extern "C" int profiles_list(profile_t *out, int capacity)
{
    std::lock_guard<std::mutex> g(lock);
    load_locked();
    int n = 0;
    for (int i = 0; i < SLOTS && n < capacity; ++i)
        if (S.slots[i].used) {
            out[n++] = S.slots[i].p;
        }
    return n;
}

extern "C" bool profiles_can(profile_cap_t cap)
{
    return unsigned(cap) < PROFILE_CAP_COUNT && (snap_caps.load() & PROFILE_CAP(cap));
}

extern "C" profile_access_t profiles_app_access(int app)
{
    if (app == A_LAUNCHER) {
        return PROFILE_ACCESS_FULL;
    }
    if (app < 0 || app >= PROFILE_APPS) {
        return snap_configured ? PROFILE_ACCESS_HIDDEN : PROFILE_ACCESS_FULL;
    }
    const uint32_t bit = 1u << app;
    return (snap_full.load() & bit) ? PROFILE_ACCESS_FULL : (snap_shown.load() & bit) ? PROFILE_ACCESS_READ : PROFILE_ACCESS_HIDDEN;
}

extern "C" bool profiles_elevated(void)
{
    return snap_elevated;
}

extern "C" bool profiles_any_elevated(void)
{
    return snap_any_elevated;
}

extern "C" void profiles_on_change(void (*fn)(void *user), void *user)
{
    std::lock_guard<std::mutex> g(lock);
    const int n = listener_count.load();
    if (!fn || n >= int(sizeof(listeners) / sizeof(listeners[0]))) {
        return;
    }
    listeners[n] = {fn, user};
    listener_count = n + 1;
}

// ---- signing in -------------------------------------------------------------------------
extern "C" uint32_t profiles_wait_s(uint8_t id)
{
    std::lock_guard<std::mutex> g(lock);
    if (id >= SLOTS || !waiting(id)) {
        return 0;
    }
    return (S.wait_until[id] - now() + 999) / 1000;
}

extern "C" int profiles_tries_left(uint8_t id)
{
    std::lock_guard<std::mutex> g(lock);
    return id < SLOTS ? std::max(0, FREE_TRIES - int(S.fails[id])) : 0;
}

extern "C" profile_result_t profiles_check_pin(uint8_t id, const char *pin)
{
    if (!valid_pin(pin)) {
        return PROFILE_ERR_INVALID;
    }
    profile_pin_t stored;
    {
        std::lock_guard<std::mutex> g(lock);
        load_locked();
        if (!S.configured || id == PROFILE_GUEST || id >= SLOTS || !S.slots[id].used) {
            return PROFILE_ERR_NO_PROFILE;
        }
        if (!S.slots[id].p.has_pin) {
            return PROFILE_ERR_NEED_PIN;
        }
        if (waiting(id)) {
            return PROFILE_ERR_WAIT;
        }
        stored = S.slots[id].pin;
    }
    if (!pbkdf2_self_test()) {
        return PROFILE_ERR_CRYPTO;
    }
    // The hash runs without the lock: it takes about half a second.
    uint8_t hash[32];
    const bool hashed = pbkdf2_sha256(reinterpret_cast<const uint8_t *>(pin), strlen(pin), stored.salt, sizeof(stored.salt),
                                      stored.iterations, hash);
    uint8_t diff = hashed ? 0 : 1;
    for (int i = 0; i < 32; ++i) {
        diff |= uint8_t(hash[i] ^ stored.hash[i]);  // every byte, whatever differs first
    }
    memset(hash, 0, sizeof(hash));
    memset(&stored, 0, sizeof(stored));
    if (!hashed) {
        return PROFILE_ERR_CRYPTO;
    }
    std::lock_guard<std::mutex> g(lock);
    if (!S.slots[id].used) {
        return PROFILE_ERR_NO_PROFILE;
    }
    if (diff) {
        count_failure(id);
        return waiting(id) ? PROFILE_ERR_WAIT : PROFILE_ERR_WRONG_PIN;
    }
    S.pin = {id, now()};
    return PROFILE_OK;
}

extern "C" void profiles_face_matched(uint8_t id)
{
    std::lock_guard<std::mutex> g(lock);
    if (id < SLOTS && S.slots[id].used) {
        S.face = {id, now()};
    }
}

extern "C" void profiles_face_failed(uint8_t id)
{
    std::lock_guard<std::mutex> g(lock);
    if (id < SLOTS && id != PROFILE_GUEST && S.slots[id].used) {
        count_failure(id);
    }
}

extern "C" profile_result_t profiles_sign_in(uint8_t id)
{
    {
        std::lock_guard<std::mutex> g(lock);
        load_locked();
        if (!S.configured || id >= SLOTS || !S.slots[id].used) {
            return PROFILE_ERR_NO_PROFILE;
        }
        const profile_t &p = S.slots[id].p;
        if (id != PROFILE_GUEST && waiting(id)) {
            return PROFILE_ERR_WAIT;
        }
        // Full mode takes both, whatever the stored sign-in says.
        const uint8_t signin = p.elevated ? uint8_t(PROFILE_SIGNIN_FACE_PIN) : p.signin;
        if (id != PROFILE_GUEST) {
            if (wants_face(signin) && !fresh(S.face, id)) {
                return PROFILE_ERR_NEED_FACE;
            }
            if (wants_pin(signin) && (!p.has_pin || !fresh(S.pin, id))) {
                return PROFILE_ERR_NEED_PIN;
            }
        }
        S.pin = S.face = Ticket{};
        if (id != PROFILE_GUEST) {
            clear_failures(id);
        }
        const bool changed = S.current != id || S.session_elevated != p.elevated;
        S.current = id;
        S.session_elevated = p.elevated && id != PROFILE_GUEST;
        S.signed_in_at = now();
        publish();
        if (!changed) {
            return PROFILE_OK;
        }
    }
    notify();
    return PROFILE_OK;
}

extern "C" void profiles_lock(void)
{
    {
        std::lock_guard<std::mutex> g(lock);
        S.pin = S.face = Ticket{};
        if (!S.configured || S.current == S.settings.default_id) {
            return;
        }
        S.current = S.settings.default_id;
        S.session_elevated = false;
        S.signed_in_at = now();
        publish();
    }
    notify();
}

extern "C" uint32_t profiles_lock_after_s(void)
{
    std::lock_guard<std::mutex> g(lock);
    if (!S.configured || S.current == S.settings.default_id) {
        return 0;
    }
    return S.slots[S.current].p.elevated ? S.settings.elevated_lock_s : S.settings.lock_s;
}

extern "C" bool profiles_idle(uint32_t idle_ms)
{
    const uint32_t limit = profiles_lock_after_s();
    if (!limit) {
        return false;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        // Unused since signing in, at most (a sign-in over USB touches nothing).
        idle_ms = std::min(idle_ms, uint32_t(now() - S.signed_in_at));
    }
    if (idle_ms < limit * 1000) {
        return false;
    }
    profiles_lock();
    return true;
}

// ---- the owner's settings ---------------------------------------------------------------
extern "C" void profiles_get_settings(profiles_settings_t *out)
{
    std::lock_guard<std::mutex> g(lock);
    load_locked();
    *out = S.settings;
}

extern "C" profile_result_t profiles_set_settings(const profiles_settings_t *settings, profile_editor_t by)
{
    {
        std::lock_guard<std::mutex> g(lock);
        load_locked();
        if (!S.configured || !may_manage(by)) {
            return PROFILE_ERR_DENIED;
        }
        const uint8_t d = settings->default_id;
        if (d >= SLOTS || !S.slots[d].used || S.slots[d].p.signin != PROFILE_SIGNIN_NONE || S.slots[d].p.elevated ||
                settings->lock_s > 3600 || settings->elevated_lock_s < 30 || settings->elevated_lock_s > 1800) {
            return PROFILE_ERR_INVALID;
        }
        const profiles_settings_t old = S.settings;
        S.settings = *settings;
        if (!save_config()) {
            S.settings = old;
            return PROFILE_ERR_STORAGE;
        }
        publish();
    }
    notify();
    return PROFILE_OK;
}

extern "C" bool profiles_hash_pin(const char *pin, profile_pin_t *out)
{
    if (!valid_pin(pin) || !out || !pbkdf2_self_test()) {
        return false;
    }
    random_bytes(out->salt, sizeof(out->salt));
    out->iterations = pbkdf2_iterations(PIN_TARGET_MS);
    return pbkdf2_sha256(reinterpret_cast<const uint8_t *>(pin), strlen(pin), out->salt, sizeof(out->salt), out->iterations,
                         out->hash);
}

extern "C" profile_result_t profiles_create_owner(const char *name, const profile_pin_t *pin, uint8_t *owner_id)
{
    {
        std::lock_guard<std::mutex> g(lock);
        load_locked();
        if (S.configured) {
            return PROFILE_ERR_DENIED;
        }
        Slot owner;
        if (!clean_name(name, owner.p.name) || !pin || !pin->iterations) {
            return PROFILE_ERR_INVALID;
        }
        reset_state();
        S.slots[PROFILE_GUEST].used = true;
        fill_defaults(PROFILE_GUEST, S.slots[PROFILE_GUEST].p);
        // Every app and every capability: the elevated ones act once the
        // owner adds a face and turns on full mode.
        char cleaned[PROFILE_NAME_LEN];
        memcpy(cleaned, owner.p.name, sizeof(cleaned));
        fill_defaults(1, owner.p);
        memcpy(owner.p.name, cleaned, sizeof(cleaned));
        owner.used = true;
        owner.p.color = 0;
        owner.p.signin = PROFILE_SIGNIN_PIN;
        owner.p.has_pin = true;
        owner.p.caps = PROFILE_CAPS_ALL;
        memset(owner.p.apps, PROFILE_ACCESS_FULL, sizeof(owner.p.apps));
        owner.pin = *pin;
        S.slots[1] = owner;
        if (!save_slot(PROFILE_GUEST) || !save_slot(1) || !save_config() || !save_fails()) {
            char key[8];
            for (uint8_t i = 0; i < 2; ++i) {
                key_for(key, i);
                store().erase(key);
            }
            reset_state();
            publish();
            return PROFILE_ERR_STORAGE;
        }
        S.configured = true;
        S.current = 1;
        S.session_elevated = false;
        S.signed_in_at = now();
        publish();
        if (owner_id) {
            *owner_id = 1;
        }
    }
    notify();
    return PROFILE_OK;
}

extern "C" profile_result_t profiles_save(profile_t *profile, profile_editor_t by)
{
    if (!profile) {
        return PROFILE_ERR_INVALID;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        load_locked();
        if (!S.configured || !may_manage(by)) {
            return PROFILE_ERR_DENIED;
        }
        uint8_t id = profile->id;
        const bool existing = id != PROFILE_NEW;
        if (!existing) {
            id = 0xFF;
            for (uint8_t i = 1; i < SLOTS && id == 0xFF; ++i)
                if (!S.slots[i].used) {
                    id = i;
                }
            if (id == 0xFF) {
                return PROFILE_ERR_FULL;
            }
        } else if (id >= SLOTS || !S.slots[id].used) {
            return PROFILE_ERR_NO_PROFILE;
        }
        Slot next = existing ? S.slots[id] : Slot{};
        next.used = true;
        profile_t &c = next.p;
        const bool had_pin = c.has_pin;
        c = *profile;
        c.id = id;
        c.has_pin = existing && had_pin;
        if (!clean_name(profile->name, c.name)) {
            return PROFILE_ERR_INVALID;
        }
        c.color = uint8_t(c.color % PROFILE_COLORS);
        c.caps &= PROFILE_CAPS_ALL;
        for (int i = 0; i < PROFILE_APPS; ++i) {
            c.apps[i] = std::min<uint8_t>(c.apps[i], PROFILE_ACCESS_FULL);
        }
        c.apps[A_LAUNCHER] = PROFILE_ACCESS_FULL;
        if (c.signin > PROFILE_SIGNIN_FACE_PIN) {
            return PROFILE_ERR_INVALID;
        }
        if (id == PROFILE_GUEST) {
            // Guest is whoever is in front of the board: no sign-in, no full
            // mode, never a manager.
            c.signin = PROFILE_SIGNIN_NONE;
            c.elevated = false;
            c.caps &= ~(MANAGE | PROFILE_CAPS_ELEVATED);
        }
        if (c.elevated && c.signin != PROFILE_SIGNIN_FACE_PIN) {
            return PROFILE_ERR_INVALID;  // full mode signs in with face and PIN
        }
        if (wants_pin(c.signin) && !c.has_pin) {
            return PROFILE_ERR_NEED_PIN;
        }
        if (wants_face(c.signin) && !face_enrolled(id)) {
            return PROFILE_ERR_NEED_FACE;
        }
        if ((c.caps & MANAGE) && !wants_pin(c.signin)) {
            return PROFILE_ERR_INVALID;  // a manager signs in with a PIN
        }
        Slot trial[SLOTS];
        std::copy(S.slots, S.slots + SLOTS, trial);
        trial[id] = next;
        if (!managers(trial)) {
            return PROFILE_ERR_LAST_MANAGER;
        }
        const Slot old = S.slots[id];
        S.slots[id] = next;
        if (!save_slot(id)) {
            S.slots[id] = old;
            return PROFILE_ERR_STORAGE;
        }
        if (!existing) {
            clear_failures(id);
        }
        const uint8_t old_default = S.settings.default_id;
        check_default();
        if (S.settings.default_id != old_default) {
            save_config();
        }
        publish();
        profile->id = id;
        profile->has_pin = c.has_pin;
    }
    notify();
    return PROFILE_OK;
}

extern "C" profile_result_t profiles_set_pin(uint8_t id, const profile_pin_t *pin, profile_editor_t by)
{
    if (!pin || !pin->iterations) {
        return PROFILE_ERR_INVALID;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        load_locked();
        if (!S.configured || id == PROFILE_GUEST || id >= SLOTS || !S.slots[id].used) {
            return id == PROFILE_GUEST ? PROFILE_ERR_INVALID : PROFILE_ERR_NO_PROFILE;
        }
        // A manager sets anyone's; everyone may change their own.
        if (!may_manage(by) && !(by == PROFILE_BY_SESSION && id == S.current)) {
            return PROFILE_ERR_DENIED;
        }
        const Slot old = S.slots[id];
        Slot &s = S.slots[id];
        s.pin = *pin;
        s.p.has_pin = true;
        if (s.p.signin == PROFILE_SIGNIN_NONE) {
            s.p.signin = PROFILE_SIGNIN_PIN;  // a PIN is set to be asked for
        }
        if (!save_slot(id)) {
            S.slots[id] = old;
            return PROFILE_ERR_STORAGE;
        }
        clear_failures(id);
        check_default();
        save_config();
        publish();
    }
    notify();
    return PROFILE_OK;
}

extern "C" profile_result_t profiles_delete(uint8_t id, profile_editor_t by)
{
    {
        std::lock_guard<std::mutex> g(lock);
        load_locked();
        if (!S.configured || !may_manage(by)) {
            return PROFILE_ERR_DENIED;
        }
        if (id == PROFILE_GUEST) {
            return PROFILE_ERR_INVALID;
        }
        if (id >= SLOTS || !S.slots[id].used) {
            return PROFILE_ERR_NO_PROFILE;
        }
        Slot trial[SLOTS];
        std::copy(S.slots, S.slots + SLOTS, trial);
        trial[id] = Slot{};
        if (!managers(trial)) {
            return PROFILE_ERR_LAST_MANAGER;
        }
        char key[8];
        key_for(key, id);
        if (!store().erase(key)) {
            return PROFILE_ERR_STORAGE;
        }
        S.slots[id] = Slot{};
        clear_failures(id);
        if (face_forget_hook) {
            face_forget_hook(id);
        }
        check_default();
        save_config();
        if (S.current == id) {
            S.current = S.settings.default_id;
            S.session_elevated = false;
            S.signed_in_at = now();
        }
        publish();
    }
    notify();
    return PROFILE_OK;
}

extern "C" profile_result_t profiles_turn_off(profile_editor_t by)
{
    {
        std::lock_guard<std::mutex> g(lock);
        load_locked();
        if (!S.configured) {
            return PROFILE_OK;
        }
        if (!may_manage(by)) {
            return PROFILE_ERR_DENIED;
        }
        char key[8];
        for (uint8_t i = 0; i < SLOTS; ++i) {
            key_for(key, i);
            store().erase(key);
            if (i && S.slots[i].used && face_forget_hook) {
                face_forget_hook(i);
            }
        }
        store().erase("cfg");
        store().erase("fails");
        reset_state();
        publish();
    }
    notify();
    return PROFILE_OK;
}

extern "C" profile_result_t profiles_unblock(uint8_t id, profile_editor_t by)
{
    std::lock_guard<std::mutex> g(lock);
    load_locked();
    if (!may_manage(by)) {
        return PROFILE_ERR_DENIED;
    }
    if (id >= SLOTS) {
        return PROFILE_ERR_NO_PROFILE;
    }
    clear_failures(id);
    return PROFILE_OK;
}

extern "C" void profiles_defaults(uint8_t id, profile_t *out)
{
    fill_defaults(id, *out);
}

extern "C" void profiles_set_face_hooks(bool (*enrolled)(uint8_t id), void (*forget)(uint8_t id))
{
    std::lock_guard<std::mutex> g(lock);
    face_enrolled_hook = enrolled;
    face_forget_hook = forget;
}

extern "C" bool profiles_face_enrolled(uint8_t id)
{
    return face_enrolled(id);
}

// ---- words -------------------------------------------------------------------------------
extern "C" const char *profiles_cap_key(profile_cap_t cap)
{
    static const char *const KEYS[PROFILE_CAP_COUNT] = {
        "home_control", "board_control", "web_search", "shortcuts_run", "shortcuts_edit", "swifty_advanced",
        "machine_actions", "paseo", "settings_network", "settings_profiles", "glimdock_actions"
    };
    return unsigned(cap) < PROFILE_CAP_COUNT ? KEYS[cap] : "";
}

extern "C" const char *profiles_cap_title(profile_cap_t cap)
{
    static const char *const TITLES[PROFILE_CAP_COUNT] = {
        "Control the home", "Board lights and IR", "Web search", "Run shortcuts", "Edit shortcuts",
        "Developer mode", "Restart machines", "Paseo agents", "Wi-Fi and VPN", "Manage profiles",
        "Change the collector"
    };
    return unsigned(cap) < PROFILE_CAP_COUNT ? TITLES[cap] : "";
}

extern "C" const char *profiles_signin_key(profile_signin_t signin)
{
    static const char *const KEYS[] = {"none", "pin", "face", "face_pin"};
    return unsigned(signin) <= PROFILE_SIGNIN_FACE_PIN ? KEYS[signin] : "";
}

extern "C" const char *profiles_result_text(profile_result_t result)
{
    switch (result) {
    case PROFILE_OK: return "Done";
    case PROFILE_ERR_WRONG_PIN: return "Wrong PIN";
    case PROFILE_ERR_WAIT: return "Too many tries: wait a moment";
    case PROFILE_ERR_NEED_PIN: return "Set a PIN first";
    case PROFILE_ERR_NEED_FACE: return "Add a face first";
    case PROFILE_ERR_NO_PROFILE: return "No such profile";
    case PROFILE_ERR_DENIED: return "Not allowed for this profile";
    case PROFILE_ERR_INVALID: return "That combination is not allowed";
    case PROFILE_ERR_LAST_MANAGER: return "Keep one profile with a PIN that manages profiles";
    case PROFILE_ERR_FULL: return "Eight profiles already";
    case PROFILE_ERR_STORAGE: return "Could not save";
    case PROFILE_ERR_CRYPTO: return "The PIN check is unavailable";
    }
    return "";
}

// ---- host tests ----------------------------------------------------------------------------
void profiles_test_platform(const profiles_platform::Store *store, uint32_t (*now_ms)())
{
    store_override = store;
    clock_override = now_ms;
}

void profiles_test_restart()
{
    std::lock_guard<std::mutex> g(lock);
    reset_state();
    S.loaded = false;
    S.signed_in_at = 0;
    publish();
}
