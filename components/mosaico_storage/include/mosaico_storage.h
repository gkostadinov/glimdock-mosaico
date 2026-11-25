// SPDX-License-Identifier: Apache-2.0
// The board's NAND at /nand: the LittleFS volume Vibe Mode keeps for Ideas.
// The volume is found and checked before anything is mounted, and it is
// never formatted: if it is missing or unlike what this firmware expects,
// storage simply stays unavailable (photos then live in RAM only).
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_STORAGE_ROOT "/nand"
// This firmware's own files; nothing outside it is written or deleted.
#define MOSAICO_STORAGE_DIR "/nand/mosaico"

// Mounts the volume (again after a pause). Call from a task with a normal
// internal stack.
bool mosaico_storage_mount(void);
// The NAND's SPI bus holds a DMA channel the camera needs to stream: the
// camera pauses storage (unmounted, bus freed) while it streams and resumes it
// after (call mosaico_storage_mount again).
void mosaico_storage_pause(bool pause);
bool mosaico_storage_paused(void);
bool mosaico_storage_ready(void);
// Writable: the volume's on-disk version matches this firmware's LittleFS, so
// writing cannot upgrade it past what Vibe Mode reads.
bool mosaico_storage_writable(void);
// Free bytes on the volume (0 when not mounted).
unsigned long long mosaico_storage_free_bytes(void);
// A short sentence for settings and the Files app.
const char *mosaico_storage_status(void);

#ifdef __cplusplus
}
#endif
