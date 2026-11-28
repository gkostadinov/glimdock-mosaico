# face_id

On-device face enrollment and recognition with the camera module, for the
profiles' sign-in. Nothing about a face leaves the board: frames, embeddings and
templates stay in its memory and NVS; there is no network code here, and the
test RPC reports only results, similarities and timings.

## How it works

- **Models**: ESP-DL 3.3 (`espressif/esp-dl`, MIT) with Espressif's
  `human_face_detect` 0.5 (MSR + MNP: a box and five landmarks) and
  `human_face_recognition` 0.3 (MobileFaceNet `mfn_s8_v1`: an aligned 112 x 112
  crop to a unit vector). Both list the ESP32-S31 and build it with the
  ESP32-P4 model files and PIE kernels. The weights are in the app image.
- **Frames**: 240 x 240 BGR888, the camera's 640 x 640 centre as the viewfinder
  shows it, upright in the UI's rotation and mirrored for the front camera,
  borrowed from the camera app (`camera_app_borrow`, below).
- **Enrollment**: five embeddings of exactly one face, near frontal, with an
  eye distance of at least 32 frame pixels (~80 cm or closer). Samples whose
  cosine with their mean is under 0.70 are dropped and more are taken. The
  normalised mean is stored per profile as int8 (key `p0`-`p15` in NVS
  namespace `face_id`, 520 bytes each). 20 s at most.
- **Identify**: per frame, the best cosine similarity over the enrolled
  templates must reach the threshold (0.55) and lead the second-best profile
  by the margin (0.06); three consecutive frames must agree (one miss is
  forgiven). Faces under 28 pixels between the eyes are asked to come closer.
- **Liveness** (on by default, `face_id_set_liveness`): the nose's position in
  the affine frame of the eyes and the mouth must move by 0.12 (on a 3-frame
  moving average) over the agreeing frames, i.e. the head turns about 15
  degrees in all. Moving, tilting or waving a flat photo is close to an affine
  map, which leaves that position unchanged. The preview says "Turn your head
  slightly" while it waits.
- **Tuning** (threshold, margin, frames, liveness, spread) is saved in NVS
  (`tune`) and can be changed over RPC 54 · 62 to calibrate on the board.

## Contract

All calls run on the LVGL task (or with the LVGL lock held). `face_id_init`
comes first (the shell calls it with a hook that runs `camera_app_start`).

- `face_id_available()`: initialised, the models usable and no camera known
  missing. Before the camera has ever been opened its presence is unknown and
  this says yes; an operation then ends with `FACE_ID_NO_CAMERA` if none answers
  within 4 s.
- `face_id_enroll_start(profile, done, user)` and
  `face_id_identify_start(timeout_ms, done, user)` return false, and never call
  `done`, for a bad profile (0-15), a null `done`, another operation already
  running or face ID unavailable. Otherwise `done` runs exactly once, later, on
  the LVGL task, never inside the start call. One operation at a time.
- `timeout_ms` 0 means 10 s; it is clamped to 1-60 s and includes the camera's
  start (~0.5-1.5 s) and, the first time, loading the models.
- `face_id_cancel()` calls `done` with `FACE_ID_CANCELLED` before it returns;
  nothing is called for that operation afterwards, so `user` may be freed
  straight after. `done` may start another operation.
- Results: `FACE_ID_OK`; `FACE_ID_BUSY` (the Camera screen, a photo for
  Swifty, the webcam or the torch has the camera, or takes it during the
  operation); `FACE_ID_NO_CAMERA`; `FACE_ID_ERROR` (no PSRAM for the models, a
  template not saved). On timeout, identify gives `FACE_ID_TIMEOUT` if a profile
  matched but the head never moved, `FACE_ID_NO_MATCH` if faces were measured
  but none matched, otherwise `FACE_ID_MANY_FACES` or `FACE_ID_NO_FACE`;
  enrollment gives `FACE_ID_TIMEOUT`, `FACE_ID_MANY_FACES` or `FACE_ID_NO_FACE`.
  Identify with nobody enrolled gives `FACE_ID_NO_MATCH` at once.
- `face_id_match_t`: on `FACE_ID_OK`, `profile` and the mean similarity of the
  agreeing frames; otherwise `profile` is 0xFF and `similarity` the best seen.
- `face_id_enrolled` / `face_id_forget`: read the cached state / erase the
  template (NVS write on the caller's task). Enrolling again replaces it.
- `face_id_preview_create(parent, x, y, size)`: a `size` x `size` round live
  picture (80-480) with a status line; delete it like any object. While it
  exists and its screen is shown it borrows the camera, and lets it go when
  hidden. One preview at a time gets frames (the newest).

## Camera borrowing (`camera_app.h`)

`camera_app_borrow` lends the camera only while no other mode uses it and
returns `CAMERA_BORROW_BUSY` otherwise. The Camera screen, Swifty's photos,
the webcam (and MosaicoCam through it) and the torch take it back at any time;
`lost` then runs once on that mode's task and `frame` is never called again.
`camera_app_borrow_pause` stops the stream (the camera closes and gives back
its internal DMA memory; the capture task ends) and restarts it.
`camera_app_borrow_end` returns once no frame is in progress. While face ID
streams, the NAND is paused as it is for the Camera (they share a DMA channel),
and the flash LED stays off whatever the Camera's flash setting.

## Threads and memory

| What | Where |
| --- | --- |
| API, borrow, NVS, `done` | LVGL task (a 66 ms timer exists only while an operation, a preview or the borrow does) |
| frame copy, preview scaling | the camera's capture task (priority 4, ~1-2 ms a frame) |
| detector and embedding | worker task `face_id`: core 1 (the S31's PIE unit is on core 1 only), priority 1, 16 KB stack in PSRAM (it never touches flash) |

Flash (and PSRAM, since the app image runs from PSRAM): ~3.06 MB, of which
ESP-DL's code 1.55 MB, MobileFaceNet 1.30 MB, the detector 0.19 MB and this
component 17 KB. Internal RAM: ~5 KB of ESP-DL statics. PSRAM at run time:
~60 KB of state; while in use the models' working memory (reported by RPC
54 · 62 as `model_kb`; 1.5 MB must be free to load them; unloaded after 60 s
idle), two 169 KB frames and 3 x size² x 3 bytes per preview (791 KB at 300 px).

ESP-DL would compile all of its ~80 operators: `CMakeLists.txt` lists the four
the three models use (Conv, Add, Concat, PRelu, plus Relu and the requantize
ops) and keeps every Conv kernel, which saves ~0.7 MB. Dropping unused Conv
kernels (the w8a16 ones are 0.5 MB) needs checking on the board first. Another
component that adds ESP-DL models must add its own `compile_req` file.

Latency (Espressif's published ESP32-S31 figures, not yet measured here): the
detector ~22 ms plus ~4 ms per candidate face, MobileFaceNet ~158 ms, so about
5 identify frames a second; three agreeing frames take ~0.6 s after the camera
starts. RPC 54 · 62 reports the measured averages.

## Limits (security)

- A 2D camera only: the liveness check stops a photo held up to the camera,
  not a video of the person turning their head played on a screen, a 3D mask
  or a photo bent around the nose. Its threshold comes from a simulation (see
  the host tests); calibrate it on the board. Elevated sign-in needs a PIN.
- MobileFaceNet: TAR 90 % at FAR 1e-4 on IJB-C (Espressif). Look-alikes and
  twins can match; the threshold and margin are starting points.
- Templates are plain NVS: the project enables neither flash nor NVS
  encryption (the board keeps Vibe Mode's bootloader and eFuses), so anyone
  who can read or write the flash can copy or replace a template. A template is
  an embedding, not a picture, but embeddings can be partly inverted.
- RPC 54 · 62 lets anyone paired with ESP-Iris enrol a face into any profile,
  forget one or change the tuning: the same trust as installing firmware.

## Test RPC (54 · 62)

See CONTRIBUTING.md. `python mosaico.py iris rpc --project projects/glimdock
18244 54 --payload-hex 3E00` reads the status (`3E` = 62).

## Host

`host/face_id_host.cpp` stands in for the core in the desktop preview: nothing
is available, and the preview widget (the board's own code) shows a synthetic
frame. The matching, agreement, liveness and template logic (`face_match.cpp`)
has host tests on synthetic embeddings and simulated landmarks:

```sh
cmake -S components/face_id/test/host -B build-host/face_id_test
cmake --build build-host/face_id_test && ctest --test-dir build-host/face_id_test --output-on-failure
```
