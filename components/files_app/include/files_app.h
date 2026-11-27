// SPDX-License-Identifier: Apache-2.0
// Files for the ESP-Mosaico shell: browse the board's NAND (camera photos and
// this firmware's files) and SMB shares on the network (configured with
// tools/mosaico-config.py, "files" section), and preview images (JPEG, PNG)
// and text files. UI calls run on the LVGL task (or with its lock held).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*exit)(void);
} files_hooks_t;

void files_app_init(const files_hooks_t *hooks);
void files_app_open(lv_obj_t *screen);
void files_app_set_active(bool active);

// The "files" configuration section: {"shares": [{"name", "host", "share",
// "user", "password", "path"}], "photos": {"share", "path"}, "swifty":
// {"share", "path"}}. Returns null
// when saved, else a reason.
const char *files_app_config_apply(const char *json, size_t length);
// The shares without their passwords (JSON).
void files_app_config_status(char *out, size_t capacity);

// Photos on the NAS: the "files" section's photos destination (a share by
// name and a folder; default the first share, "Mosaico Photos"). Blocking
// network calls: not on the LVGL task where avoidable.
typedef struct {
    char name[40];
    uint32_t size, mtime;
} files_photo_t;
bool files_nas_ready(void);
bool files_nas_put(const char *name, const uint8_t *data, size_t size);
int files_nas_list(files_photo_t *out, int max);  // -1 when the share is unreachable
bool files_nas_get(const char *name, uint8_t **data, size_t *size);  // free with heap_caps_free
bool files_nas_delete(const char *name);

typedef struct {
    bool storage;
    int shares;
    char status[64];
} files_summary_t;
void files_app_summary(files_summary_t *out);

// For Swifty (blocking: not on the LVGL task). Paths start with a place
// ("Photos", "This board" or a share's name), e.g. "Media/Videos". An empty
// path lists the places. Read gives a text file's start.
void files_app_tool_list(const char *path, char *out, size_t capacity);
void files_app_tool_read(const char *path, char *out, size_t capacity);
// Swifty's own folder ("Swifty" among the places; the "files" section's
// "swifty": {"share", "path"}, by default the photos share, "Swifty"): the
// only place it may write. Paths are inside it ("notes/shopping.md", or with
// "Swifty/" in front). Write replaces the file, or appends to it; text kinds
// only, 16 KB a write, 256 KB a file. Delete takes a file or an empty folder.
void files_app_tool_write(const char *path, const char *text, bool append, char *out, size_t capacity);
void files_app_tool_delete(const char *path, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
