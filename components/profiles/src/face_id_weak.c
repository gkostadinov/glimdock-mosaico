// SPDX-License-Identifier: Apache-2.0
// Stand-ins for components/face_id (face_id.h) while it is not in the build:
// no camera, so face sign-in reports itself unavailable and the profiles UI
// says it needs the camera module. Weak, so the real component's definitions
// win; CMakeLists.txt also leaves this file out once components/face_id exists.
#include "face_id.h"

#include <stddef.h>

__attribute__((weak)) bool face_id_available(void)
{
    return false;
}

__attribute__((weak)) bool face_id_enroll_start(uint8_t profile, void (*done)(face_id_result_t, void *user), void *user)
{
    (void)profile;
    (void)done;
    (void)user;
    return false;
}

__attribute__((weak)) bool face_id_identify_start(uint32_t timeout_ms, void (*done)(face_id_match_t, void *user), void *user)
{
    (void)timeout_ms;
    (void)done;
    (void)user;
    return false;
}

__attribute__((weak)) void face_id_cancel(void) {}

__attribute__((weak)) bool face_id_enrolled(uint8_t profile)
{
    (void)profile;
    return false;
}

__attribute__((weak)) void face_id_forget(uint8_t profile)
{
    (void)profile;
}

__attribute__((weak)) lv_obj_t *face_id_preview_create(lv_obj_t *parent, int x, int y, int size)
{
    (void)parent;
    (void)x;
    (void)y;
    (void)size;
    return NULL;
}
