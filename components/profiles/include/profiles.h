// SPDX-License-Identifier: Apache-2.0
// Profiles: who is using the board and what they may do. Up to eight
// profiles plus Guest, each with a name, a colour, a way to sign in (none, a
// PIN, a face, or both) and the owner's choices: every app hidden, read-only
// or full, and capabilities (profile_cap_t). An elevated profile ("full
// mode") signs in only with both its face and its PIN, and only it holds the
// dangerous capabilities (PROFILE_CAPS_ELEVATED).
//
// With no profiles set up the board behaves as it always has: one implicit
// owner who may do everything but developer mode (which needs full mode), and
// nothing locks.
//
// Gate cheaply anywhere with profiles_can() and profiles_app_access(): they
// read a snapshot and are safe from any task. Changes (signing in, locking,
// the owner's edits) run on the LVGL task or with the display lock held, and
// call the listeners there. Kept in NVS ("profiles"); a PIN only as a salted
// PBKDF2-HMAC-SHA256 hash.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILES_MAX 8         // profiles besides Guest (ids 1-8)
#define PROFILE_GUEST 0        // always there once profiles are set up; never signs in
#define PROFILE_IMPLICIT 0xFE  // no profiles set up: the board's owner, as before
#define PROFILE_NEW 0xFF       // profiles_save: make a new one
#define PROFILE_NAME_LEN 20    // bytes, the NUL included
#define PROFILE_APPS 32        // app slots: a ShellApp's value, or PROFILE_APP_SETTINGS
#define PROFILE_APP_SETTINGS 31  // the Settings sheet (not a ShellApp)
#define PROFILE_PIN_MIN 4
#define PROFILE_PIN_MAX 6
#define PROFILE_COLORS 8       // colour indices the UI draws

typedef enum {
    PROFILE_SIGNIN_NONE = 0,  // a tap (Guest; or a profile the owner leaves open)
    PROFILE_SIGNIN_PIN,
    PROFILE_SIGNIN_FACE,
    PROFILE_SIGNIN_FACE_PIN,  // what full mode requires
} profile_signin_t;

typedef enum {
    PROFILE_ACCESS_HIDDEN = 0,  // not on the home screen, refused when opened
    PROFILE_ACCESS_READ,        // shown, changes refused (Home, Glimdock, Swifty's tools)
    PROFILE_ACCESS_FULL,
} profile_access_t;

// Values are stored as bit positions: append, never reorder.
typedef enum {
    PROFILE_CAP_HOME_CONTROL = 0,   // lights, plugs, TV, music (Home, Swifty)
    PROFILE_CAP_BOARD_CONTROL,      // the Interaction board's lights and IR, the flashlight
    PROFILE_CAP_WEB_SEARCH,         // Swifty's web search
    PROFILE_CAP_SHORTCUTS_RUN,      // shortcut tiles and run_shortcut
    PROFILE_CAP_SHORTCUTS_EDIT,     // the shortcuts' editor, add/delete_shortcut
    PROFILE_CAP_SWIFTY_ADVANCED,    // developer mode: code, longer answers (full mode)
    PROFILE_CAP_MACHINE_ACTIONS,    // restart or power machines (full mode)
    PROFILE_CAP_PASEO,              // Swifty talks to Paseo agents (full mode)
    PROFILE_CAP_SETTINGS_NETWORK,   // Wi-Fi, the VPN, web settings
    PROFILE_CAP_SETTINGS_PROFILES,  // Settings > Profiles (needs a PIN; full mode once one exists)
    PROFILE_CAP_GLIMDOCK_ACTIONS,   // change the collector: Glimdock's connection and nodes (full mode)
    PROFILE_CAP_COUNT
} profile_cap_t;

#define PROFILE_CAP(c) (1u << (c))
#define PROFILE_CAPS_ALL ((1u << PROFILE_CAP_COUNT) - 1u)
// Held only in full mode: a profile may have them ticked, they act only while
// it is signed in with its face and PIN.
#define PROFILE_CAPS_ELEVATED                                                                     \
    (PROFILE_CAP(PROFILE_CAP_SWIFTY_ADVANCED) | PROFILE_CAP(PROFILE_CAP_MACHINE_ACTIONS) |      \
     PROFILE_CAP(PROFILE_CAP_PASEO) | PROFILE_CAP(PROFILE_CAP_GLIMDOCK_ACTIONS))

typedef struct {
    uint8_t id;                  // PROFILE_GUEST, 1-8, or PROFILE_IMPLICIT
    char name[PROFILE_NAME_LEN];
    uint8_t color;               // 0 to PROFILE_COLORS - 1
    uint8_t signin;              // profile_signin_t
    bool elevated;               // full mode: signs in with face and PIN
    bool has_pin;                // read only: set with profiles_set_pin
    uint32_t caps;               // PROFILE_CAP() bits the owner ticked
    uint8_t apps[PROFILE_APPS];  // profile_access_t per app slot
} profile_t;

typedef enum {
    PROFILE_OK = 0,
    PROFILE_ERR_WRONG_PIN,
    PROFILE_ERR_WAIT,          // too many wrong tries: profiles_wait_s()
    PROFILE_ERR_NEED_PIN,      // the PIN is not checked yet, or the profile has none
    PROFILE_ERR_NEED_FACE,     // the face is not matched yet, or not enrolled
    PROFILE_ERR_NO_PROFILE,
    PROFILE_ERR_DENIED,        // this session may not make that change
    PROFILE_ERR_INVALID,       // a bad name, PIN or combination
    PROFILE_ERR_LAST_MANAGER,  // keep one profile that can manage profiles, with a PIN
    PROFILE_ERR_FULL,          // eight profiles already
    PROFILE_ERR_STORAGE,
    PROFILE_ERR_CRYPTO,        // the PIN hash failed its self-test
} profile_result_t;

// Who asks for a change: the person at the board (their session's rights), or
// a trusted channel (USB, the recovery RPC) that may manage profiles without
// signing in. The model's own rules (a PIN for managers, face and PIN for
// full mode, one manager kept) hold for both.
typedef enum { PROFILE_BY_SESSION = 0, PROFILE_BY_TRUSTED } profile_editor_t;

typedef struct {
    uint8_t default_id;        // signed in when locked: Guest or another profile without sign-in
    uint16_t lock_s;           // a signed-in profile locks after this long unused (0: never)
    uint16_t elevated_lock_s;  // full mode locks sooner (never 0)
} profiles_settings_t;

// A PIN as stored: never the digits themselves.
typedef struct {
    uint8_t salt[16];
    uint8_t hash[32];
    uint32_t iterations;
} profile_pin_t;

// ---- state -------------------------------------------------------------------------------
// Loads NVS once (the first profile signed in is the default one). Call
// before anything reads profiles; later calls do nothing.
void profiles_init(void);
bool profiles_configured(void);
uint8_t profiles_current(void);  // PROFILE_IMPLICIT while not set up
// A profile by id (PROFILE_IMPLICIT too); false when there is none.
bool profiles_get(uint8_t id, profile_t *out);
// Every profile, by id (Guest first); returns how many.
int profiles_list(profile_t *out, int capacity);
// The session holds this capability now.
bool profiles_can(profile_cap_t cap);
// What the session may do with an app slot (a ShellApp's value). Slots past
// PROFILE_APPS are hidden for set-up profiles.
profile_access_t profiles_app_access(int app);
// The session is in full mode (an elevated profile signed in with face and PIN).
bool profiles_elevated(void);
// Whether any profile is elevated (then only full mode manages profiles).
bool profiles_any_elevated(void);
// Called after any change of who is signed in or of what they may do (up to 8).
void profiles_on_change(void (*fn)(void *user), void *user);

// ---- signing in -------------------------------------------------------------------------
// Seconds before the next try (0: may try now), and wrong tries left before a wait.
uint32_t profiles_wait_s(uint8_t id);
int profiles_tries_left(uint8_t id);
// Checks a PIN (any task; blocks for the hash, ~0.5 s). Right: remembered for a
// minute, for profiles_sign_in. Wrong: counted, and a wait grows with each.
profile_result_t profiles_check_pin(uint8_t id, const char *pin);
// face_id matched this profile's face just now (its done callback calls this).
void profiles_face_matched(uint8_t id);
// A face that did not match counts as a wrong try (face sign-in has a limit too).
void profiles_face_failed(uint8_t id);
// Signs in with what its sign-in needs: a face matched and a PIN checked in
// the last minute. Both are used up.
profile_result_t profiles_sign_in(uint8_t id);
// Back to the default profile.
void profiles_lock(void);
// The shell's timer: the board unused for idle_ms. True when it just locked.
bool profiles_idle(uint32_t idle_ms);
// Seconds until the session locks if nothing is touched (0: it does not).
uint32_t profiles_lock_after_s(void);

// ---- the owner's settings ---------------------------------------------------------------
void profiles_get_settings(profiles_settings_t *out);
profile_result_t profiles_set_settings(const profiles_settings_t *settings, profile_editor_t by);
// Hashes a PIN of 4-6 digits with a new salt (any task; ~0.5 s). False for a
// bad PIN or when the hash's self-test failed.
bool profiles_hash_pin(const char *pin, profile_pin_t *out);
// First set-up, while no profiles exist: Guest (the defaults below) and an
// owner with every app, every capability and this PIN, signed in at once.
profile_result_t profiles_create_owner(const char *name, const profile_pin_t *pin, uint8_t *owner_id);
// Creates (id PROFILE_NEW; the new id is written back) or changes a profile.
// has_pin is ignored: profiles_set_pin sets it.
profile_result_t profiles_save(profile_t *profile, profile_editor_t by);
profile_result_t profiles_set_pin(uint8_t id, const profile_pin_t *pin, profile_editor_t by);
profile_result_t profiles_delete(uint8_t id, profile_editor_t by);
// Every profile and face gone: the board as before profiles.
profile_result_t profiles_turn_off(profile_editor_t by);
// Forgets wrong tries (recovery).
profile_result_t profiles_unblock(uint8_t id, profile_editor_t by);
// What a new profile starts with (Guest's defaults for PROFILE_GUEST).
void profiles_defaults(uint8_t id, profile_t *out);

// Face enrolment, from the shell (face_id.h): whether a profile has a face,
// and forgetting one when its profile goes. Unset: no faces.
void profiles_set_face_hooks(bool (*enrolled)(uint8_t id), void (*forget)(uint8_t id));
bool profiles_face_enrolled(uint8_t id);

// ---- words -------------------------------------------------------------------------------
const char *profiles_cap_key(profile_cap_t cap);    // "home_control"
const char *profiles_cap_title(profile_cap_t cap);  // "Control the home"
const char *profiles_signin_key(profile_signin_t signin);  // "none", "pin", "face", "face_pin"
const char *profiles_result_text(profile_result_t result);

#ifdef __cplusplus
}

// For C++: profiles_can(Capability::paseo), profiles_app_access(ShellApp::Home).
enum class Capability : uint8_t {
    home_control = PROFILE_CAP_HOME_CONTROL,
    board_control = PROFILE_CAP_BOARD_CONTROL,
    web_search = PROFILE_CAP_WEB_SEARCH,
    shortcuts_run = PROFILE_CAP_SHORTCUTS_RUN,
    shortcuts_edit = PROFILE_CAP_SHORTCUTS_EDIT,
    swifty_advanced = PROFILE_CAP_SWIFTY_ADVANCED,
    machine_actions = PROFILE_CAP_MACHINE_ACTIONS,
    paseo = PROFILE_CAP_PASEO,
    settings_network = PROFILE_CAP_SETTINGS_NETWORK,
    settings_profiles = PROFILE_CAP_SETTINGS_PROFILES,
    glimdock_actions = PROFILE_CAP_GLIMDOCK_ACTIONS,
};
inline bool profiles_can(Capability cap)
{
    return profiles_can(profile_cap_t(cap));
}

// ShellApp and other app enums straight in.
template <typename App> inline profile_access_t profiles_app_access(App app)
{
    return profiles_app_access(int(app));
}
#endif
