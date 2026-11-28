// SPDX-License-Identifier: Apache-2.0
// Face sign-in (components/face_id, built alongside): this is a private copy
// of its interface so profiles builds on its own; src/face_id_weak.c stands in
// until the real component is in the build. Keep it identical to the
// component's header.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FACE_ID_OK = 0, FACE_ID_NO_CAMERA, FACE_ID_NO_FACE, FACE_ID_MANY_FACES, FACE_ID_NO_MATCH,
               FACE_ID_BUSY, FACE_ID_CANCELLED, FACE_ID_TIMEOUT, FACE_ID_ERROR
             } face_id_result_t;
typedef struct {
    face_id_result_t result;
    uint8_t profile;
    float similarity;
} face_id_match_t;
bool face_id_available(void);   // camera module present and model ready
bool face_id_enroll_start(uint8_t profile, void (*done)(face_id_result_t, void *user), void *user);  // done on the LVGL task
bool face_id_identify_start(uint32_t timeout_ms, void (*done)(face_id_match_t, void *user), void *user);  // done on the LVGL task
void face_id_cancel(void);
bool face_id_enrolled(uint8_t profile);
void face_id_forget(uint8_t profile);
lv_obj_t *face_id_preview_create(lv_obj_t *parent, int x, int y, int size);  // live round preview; LVGL task

#ifdef __cplusplus
}
#endif
