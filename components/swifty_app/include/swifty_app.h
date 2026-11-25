// SPDX-License-Identifier: Apache-2.0
// Swifty on the ESP-Mosaico: Swifty Home's bird in 3D, a chat with an
// OpenAI-compatible model (Markdown replies, camera photos in and out, web
// images), and push-to-talk through the Swifty speech service. UI calls run
// on the LVGL task (or with its lock held).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*exit)(void);          // back to the launcher
    void (*camera_ready)(void);  // make sure the camera app is started (camera_app_start)
    void (*pick_photo)(void);    // open the Camera viewfinder; the shot comes back via swifty_app_attach_photo
} swifty_hooks_t;

// Loads the configuration and registers its RPC; call once at start-up.
void swifty_app_init(const swifty_hooks_t *hooks);
// First call builds the UI on screen (a full-panel LVGL screen).
void swifty_app_open(lv_obj_t *screen);
void swifty_app_set_active(bool active);
// Push-to-talk from the AI key: pressed starts listening, released sends.
void swifty_app_talk(bool pressed);
// Attach a camera photo (by index) to the next message.
void swifty_app_attach_photo(int index);

typedef struct {
    bool configured, busy, listening;
    char status[64];
    char last[120];  // the start of Swifty's last reply
} swifty_summary_t;
void swifty_app_summary(swifty_summary_t *out);

// Other apps' sections of the board configuration, loaded together with
// Swifty's by tools/mosaico-config.py (service 0x4744 method 40).
// apply gets the section as JSON and returns NULL, "restart" (saved; applies
// after a restart) or an error; status writes the section's state as JSON,
// never secrets.
typedef const char *(*swifty_section_apply_t)(const char *json, size_t length);
typedef void (*swifty_section_status_t)(char *json, size_t capacity);
void swifty_app_config_section(const char *name, swifty_section_apply_t apply, swifty_section_status_t status);

// Tools other apps give Swifty. parameters_json is a JSON-schema object
// (e.g. {"type":"object","properties":{...}}). fn runs on Swifty's agent task
// with the call's arguments as JSON text and writes a short text result; it
// must take the display lock (bsp_display_lock) for anything on the UI.
typedef void (*swifty_tool_fn)(const char *args_json, char *out, size_t capacity);
void swifty_app_add_tool(const char *name, const char *description, const char *parameters_json, swifty_tool_fn fn);

// A still of Swifty (ARGB8888, size x size) for the launcher; rendered once.
const lv_image_dsc_t *swifty_app_portrait(int size);

#ifdef __cplusplus
}
#endif
