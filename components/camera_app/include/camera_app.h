// SPDX-License-Identifier: Apache-2.0
// Camera for the ESP-Mosaico shell: live viewfinder from the camera sub-board
// (left slot), photos on the board's NAND (or PSRAM), a gallery, and an
// ESP-Iris RPC that lets tools/camera-photos.py download them to a Mac.
// UI calls run on the LVGL task (or with its lock held).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// First call builds the UI on screen (a full-panel LVGL screen) and starts
// the capture task; the camera streams only while the app is active.
void camera_app_open(lv_obj_t *screen);
// Builds the UI and starts the capture task without streaming (for Swifty).
void camera_app_start(lv_obj_t *screen);
void camera_app_set_active(bool active);
// Torch for the Tools app: lights the module's flash with the camera open but
// not streaming (after camera_app_start). Returns false without a camera
// module (it waits up to ~3 s for the module to open).
bool camera_app_torch(bool on);
// Panel brightness for the screen flash (front camera): a percent, -1 restores.
void camera_app_set_brightness_hook(void (*set)(int percent));
// Stops streaming before a software restart (camera DMA must be idle).
void camera_app_shutdown(void);

typedef struct {
    bool started, present, streaming;
    int photos;
    char status[48];
} camera_summary_t;
void camera_app_summary(camera_summary_t *out);

// For Swifty. Takes a photo with the Camera screen hidden (after
// camera_app_start); blocks up to timeout_ms; returns its index or -1.
int camera_app_take_photo(uint32_t timeout_ms);
// The next photo taken on the Camera screen goes to done (LVGL task) instead
// of only the gallery: Swifty's camera button opens the viewfinder this way.
void camera_app_pick(void (*done)(int index));
int camera_app_photo_count(void);
// A copy of a photo's JPEG (index < 0 counts back from the newest), allocated
// with heap_caps_malloc; the caller frees it.
bool camera_app_photo_jpeg(int index, uint8_t **jpeg, size_t *size);
// For the Gallery app: a photo's time (Unix, 0 unknown) and whether it is on
// the NAS; deleting (memory and NAS); a tick that changes when photos do.
bool camera_app_photo_info(int index, uint32_t *at, bool *on_nas, uint32_t *size);
bool camera_app_delete_photo(int index);
uint32_t camera_app_photos_revision(void);
// Lists the NAS photo folder again (the Gallery opening); new files join the list.
void camera_app_refresh_photos(void);
// USB webcam frames: JPEG of what the camera sees, handed to sink on the
// capture task for every frame; the camera keeps streaming while a sink is
// set. Null stops. The camera must have been started (camera_app_start).
// view, asked every frame: the size to send (0 x 0: nobody watching, no frame)
// and which way is up, as a UI rotation (0, 90, 180, 270). The picture is the
// true view of the current front/rear (unmirrored, as webcams send it: the
// computer mirrors its own preview); the flip button mirrors it.
typedef void (*camera_webcam_view_t)(int *width, int *height, int *rotation);
void camera_app_set_frame_sink(void (*sink)(const unsigned char *jpeg, size_t length), camera_webcam_view_t view);
// Webcam frames made and the time each stage took (averages), as text.
void camera_app_webcam_stats(char *out, size_t capacity, bool reset);
// Webcam frames through YUV420 (the default) or RGB565 between PPA and JPEG.
void camera_app_webcam_yuv(bool on);
// Whether the camera works out front or rear by itself (the module turning, a
// quarter turn of the board); off, only the front/rear button changes it.
// Saved (NVS camera/autoface), on by default.
void camera_app_set_auto_facing(bool on);
bool camera_app_auto_facing(void);
// The NAS list: 0 listing, 1 listed, -1 unreachable, -2 no share set up.
int camera_app_photos_state(void);

// Face ID (components/face_id) borrows the camera while no other mode uses it.
// Frames are square BGR888 (the PPA's RGB888 byte order), the sensor's centre
// as the viewfinder shows it, upright in the UI's rotation and mirrored for the
// front camera (as a person sees themselves). frame runs on the capture task
// with the borrow's lock held: copy and return. Streaming pauses the NAND, as
// the Camera does. Needs camera_app_start first.
typedef struct {
    int size;  // side in pixels, a multiple of 40 from 80 to 640 (an exact PPA scale of the 640 crop)
    void (*frame)(const uint8_t *bgr, int size, bool rear, void *user);
    // Another mode (the Camera screen, a photo for Swifty, the webcam, the
    // torch) took the camera: the borrow is over. Called once, on that
    // mode's task, after which frame is never called again.
    void (*lost)(void *user);
    void *user;
} camera_borrow_t;
typedef enum { CAMERA_BORROW_OK, CAMERA_BORROW_BUSY, CAMERA_BORROW_NOT_STARTED, CAMERA_BORROW_INVALID } camera_borrow_result_t;
// BUSY while another mode owns the camera (nothing changes then).
camera_borrow_result_t camera_app_borrow(const camera_borrow_t *borrow);
// Stops the stream (the camera closes and its DMA memory comes back) and
// starts it again, keeping the borrow.
void camera_app_borrow_pause(bool paused);
// Ends the borrow; frame and lost are never called once this returns.
void camera_app_borrow_end(void);
// 1 a camera module answered, 0 none did when last opened, -1 never opened.
int camera_app_present(void);
// The viewfinder's thumbnail opens this instead of the built-in viewer.
void camera_app_set_gallery_hook(void (*open)(void));

// Gallery (gallery.cpp): the photos as a grid, and one at a time full screen.
void gallery_app_open(lv_obj_t *screen);
void gallery_app_set_active(bool active);
void gallery_app_set_exit(void (*exit)(void));
// Show this photo full screen when the Gallery opens (-1 newest, -2 the grid).
void gallery_app_show(int index);

#ifdef __cplusplus
}
#endif
