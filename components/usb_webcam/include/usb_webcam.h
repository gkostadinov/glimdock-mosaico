// SPDX-License-Identifier: Apache-2.0
// USB webcam mode. start: ESP-Iris stops (no USB console, installs or Wi-Fi
// tools meanwhile) and the USB port becomes a UVC camera: MJPEG, 1280 x 720 or
// 640 x 480 at 25 fps (the sensor's rate).
// stop: the camera goes and ESP-Iris comes back. Frames come from push().
// start and stop block (up to a few seconds): not on the LVGL task.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_WEBCAM_FPS 25

esp_err_t usb_webcam_start(void);
esp_err_t usb_webcam_stop(void);
bool usb_webcam_active(void);     // the USB port is the camera
// From the start of usb_webcam_start to the end of usb_webcam_stop: TinyUSB
// is not ESP-Iris's, so nothing else may reconnect or query the port.
bool usb_webcam_owns_usb(void);
bool usb_webcam_streaming(void);  // a computer has the stream open
// The frame size the computer chose (the first one until it chooses).
void usb_webcam_format(int *width, int *height);
// A JPEG frame (copied; the latest one wins). Any task.
void usb_webcam_push(const uint8_t *jpeg, size_t length);
// Bytes of the latest pushed JPEG from offset (kept after the webcam stops),
// for checking a frame; 0 past its end.
size_t usb_webcam_last_frame(size_t offset, uint8_t *out, size_t capacity);
// Frames sent in the last second.
int usb_webcam_fps(void);
// What the last webcam session saw (for checking afterwards, when ESP-Iris is
// back): configured by the host, stream commits, frames sent, frames pushed.
void usb_webcam_report(char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
