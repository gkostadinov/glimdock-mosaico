# Contributing

Issues and pull requests are welcome. Please keep changes focused, describe how
you tested them on a board, and match the surrounding style (`tools/quality`
holds the checks).

## Ground rules

- **Never write outside the application slot.** The bootloader, Vibe Mode, its
  settings (`sysmeta`) and the NAND volume belong to the board. Install with
  `mosaico.py iris app-update`; don't erase flash, and never format the NAND
  (`components/mosaico_storage` mounts it read-write without formatting).
- **Keep NVS.** It holds Glimdock and Home pairings and every app's settings.
- **No private values in the tree.** Servers, keys and Wi-Fi credentials come
  from the board's settings (`tools/mosaico-config.py`), never from source.
  Glimdock's private build defaults (`local_credentials.h`) are excluded by
  `GLIMDOCK_PUBLIC_FIRMWARE`.
- **Restart through ESP-Iris** (`esp_iris_mark_planned_restart()` and
  `esp_iris_restart_now()`): it resets the S31's USB so the board comes back on
  USB afterwards.
- **Shutdown handlers must not block**; a blocked restart ends by watchdog and
  leaves USB DMA running into the next boot.
- **Keep the 1 kHz FreeRTOS tick** (`sdkconfig.defaults`). At 100 Hz any wait
  under 10 ms rounds to 0 ticks and spins: the LVGL adapter's "sleep until the
  next timer" took ~45 % of a core at priority 6. Prefer notifications to
  polling either way.
- **Internal RAM is the scarce memory.** `malloc()` and `new` go to PSRAM
  first (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`); ask for internal or DMA
  memory with `heap_caps_malloc` only where it is needed. TLS handshakes, task
  stacks and the display's transfers need it: when it ran out, the speech
  connection failed and retried until the board slowed down. Give tasks that
  never touch flash a PSRAM stack (`xTaskCreatePinnedToCoreWithCaps`), and
  check what each app keeps with RPC 16. A task that needs an internal stack
  but works only now and then can end when idle and be started again: the
  Camera's capture task does that (8 KB back while no camera is in use). The
  board's module manager (`module_scan`, `module_event`, 4 KB each, in the
  vendor BSP) stays for good once a module is first used.
- **Priorities:** tasks at or above LVGL's draw threads (priority 3) can starve
  the UI. Keep background work at 2 or below unless it blocks quickly. Swifty's
  bird renders at 1, below network and TLS work (2); its 10-second log line
  shows where each frame's time goes. Don't hold a mutex across slow work: a
  moment's contention raises the holder to the waiter's priority, and FreeRTOS
  keeps it raised until the task holds no mutex at all (the speech socket's TLS
  connect ran at 18-23 and froze the screen; it now runs in a helper task). A
  frame over 500 ms logs the running tasks with their base priorities.

## Keeping the UI fast

Measure on the board (RPC 11, 14, 15 and 16 below) before and after a change.
What the measurements taught so far:

- **LVGL redraws on every set**, of the same value too. Periodic updaters use
  `mui::set_text`, `set_hidden`, `set_bg_color` and the other `set_*`
  helpers, which leave an unchanged object alone; `bar_set` does the same.
- **Never lay the screen out while building a page.** `lv_obj_update_layout`
  lays out the whole screen; reading a size that way in a per-object helper
  made a page build quadratic. Read the size as set, or measure text with
  `lv_text_get_size`.
- **Fonts come from `mui::font`**, whose wrapper caches glyph metrics and
  kerning in front of tiny_ttf (its own lookups cost ~24 us a glyph).
- **Update pages in place when their shape is unchanged** (Glimdock's
  Systems page): building costs ~125 us an object on the PSRAM heap, plus
  layout and a whole-screen redraw.
- **Redraw on what is shown changing**: a clock showing minutes redraws a
  minute apart; a sensor's noise stays under a dead band.
- The display sends whole frames behind the panel's scan while LVGL renders
  the next one (`frame_sync.cpp`); a full-screen change reaches ~30 fps, the
  40 MHz QSPI link's limit.
- **The games share one engine** (`components/games_app/src/games_3d.h`): a
  camera with near-plane clipping, flat-shaded meshes drawn far faces first,
  and particles, over the frame and raster helpers of `games_common.h`. A new
  game draws through it; a ray caster or voxel terrain draws its world only
  where that is faster, with the engine's meshes on top.
- **Games are bound by PSRAM bandwidth, not the CPU.** Each game draws a
  240 × 240 frame (three buffers in PSRAM) in its own task, and the frame's
  object doubles it straight into LVGL's buffer while LVGL draws: a 480 × 480
  image in between cost a whole extra write and copy per frame. The ray caster
  and the voxel terrain draw column by column into a transposed frame, so
  their writes run along memory, and the voxel map keeps height and colour
  side by side. The 10-second `games:` log line gives each game's drawing time
  by section; all six run at about 30 fps.

## Checks

```sh
python3 -m pip install -r tools/quality/requirements.txt
pre-commit install
pre-commit run --all-files
```

The project hooks run the ESP-Mosaico toolkit's checks from the workspace
(`../../submodule/esp-mosaico-utils`).

## Diagnostics over ESP-Iris

The shell registers RPCs on service `0x4744` (18244), callable with
`python mosaico.py iris rpc --project projects/glimdock 18244 METHOD [--payload-hex HEX]`:

| Method | Payload | Reply |
| --- | --- | --- |
| 10 | none, or `01 r` / `02 e` / `03 m` | Orientation: accelerometer, gravity, rotation; force a rotation, toggle auto-rotate or set the axis map. While a game is played: the gyroscope, the turn about the vertical, the gyro's learned sense and its rest bias. |
| 11 | `01` resets | Frame statistics: frames, render, flush-wait and layout times. |
| 13, 14 | none | Mark, then CPU per task since the mark. |
| 15 | seconds (starts) / none (result) | Time spent in every LVGL timer callback, by address (symbolise with `addr2line`). |
| 16 | none | Internal and DMA memory (free, largest block, lowest, blocks) and every task whose stack is in internal RAM, with its unused bytes. |
| 24 | none | Camera facing and why. |
| 30 | app (`00` launcher … `07` Gallery, `08` Webcam, `09` Interaction, `0A` Calculator, `0B` VPN, `0C` Games), then a Tools tab (`00` torch, `01` level, `02` compass; the old `03`–`05` open Calculator, Interaction and Webcam), a Glimdock page (`00` Systems, `01` Clock, … `0A` Agents) or a game's title screen (`00` Ember Crypt … `05` Ridge Rally, `FF` the picker) | Opens an app. |
| 40 | op `00` status; `01` begin (u32 size), `02` chunk, `03` commit, `04` abort; `05` status from an offset (u32) | The board configuration that `tools/mosaico-config.py` reads and writes. The status is longer than one reply: read it with `05` a page at a time until an empty page. |
| 41, 42 | text / `01`, `00` | Swifty says something / listens. |
| 43 | seconds, `01` (encode with no computer watching), rotation ÷ 90 + 1, `02` (YUV420) | A timed USB webcam session that ends by itself. |
| 44 | none | The last webcam session: USB setup, stream commits, frames, the microphone (reads, packets, empty packets, peaks, the computer's input level), the latest control requests. |
| 45 | none | The last webcam session's effect on the board: LVGL loop gaps, CPU windows, time per frame by stage. |
| 46 | 32-bit offset (little-endian) | The last webcam JPEG, 960 bytes at a time. |
| 47 | `01` held / `00` released / `02` a real press (down, then held 300 ms later) / `03` a touch / `04` a click (down, the click 250 ms later) | As the AI key: opens Swifty when needed and talks. The log times each step (Swifty shown, microphone open, connection, recognition). `03` and `04` wake the always-on display as a touch and the key do. |
| 48 | keyframes of 11 bytes, little-endian: ms from the start (u16), fingers down (0–2), then x, y of each finger (i16) | A scripted touch, for testing gestures such as a pinch without a finger on the glass. Points move in a straight line between keyframes with the same fingers; the panel is ignored until the last keyframe. |
| 49 | none / `01` / `02` / `00` | The always-on display: its state (off, dimmed or showing), timeout, seconds without use, times shown, clock draws and times dimmed (nothing is logged during webcam mode, where it only dims); `01` shows the clock now, `02` dims, `00` wakes the board. |
| 52 | a URL (`http://` or `https://`) / none | Starts a GET of the URL / its result: HTTP status, time, the default route and DNS servers in use, the start of the body. Where the board's traffic goes, with the VPN on or off. |
| 55 | none, or JSON text: `{"op":"list"}`, `{"op":"run","id":N,"dry_run":true}`, `{"op":"save","shortcut":{...}}`, `{"op":"delete","id":N}`, `{"op":"starters"}`, `{"op":"editor"}`, `{"op":"dry_run","on":true}` | Shortcuts (`shortcuts.h`): the list with each one's steps and the last run, or a change. `dry_run` on makes every run a dry run (nothing sent to Swifty Home or the Interaction board) until a restart. |

| 53 | none / `00` / `01` then text / `02` / `03` / `04` / `05` / `06` | The home screen: page, editing and layout (pages split by `\|`, keys by commas, hidden keys after `#`). `00` resets the layout, `01` sets it from the text, `02` and `03` start and end editing, `04` shows the hidden apps, `05` shows Settings and `06` opens Settings > Wi-Fi. The reply comes before the change. |
| 56 | `00` / `01` / `02` ssid `00` password / `03` ssid | The board's Wi-Fi (`board_wifi.h`) as JSON: the network, whether it is connected, its address and signal, whether it is saved or Vibe Mode's, the last join's result and the networks from the last scan. `01` scans first (read again for the results), `02` joins (no password: the saved one), `03` forgets a saved network. Passwords are never in the reply. |
| 54 · 60 | none or `{"op":"list","from":N}`; or JSON: `{"op":"setup","name":"Owner","pin":"2468"}`, `{"op":"save","profile":{"id":N,"name":"Partner","signin":"pin","caps":["home_control"],"apps":{"glimdock":"hidden","home":"full"}}}` (no id: a new one), `{"op":"delete","id":N}`, `{"op":"lock"}`, `{"op":"switch","id":N,"pin":"1357"}`, `{"op":"settings","default":0,"lock_s":300,"elevated_lock_s":120}` | Profiles (`profiles.h`) as JSON, a page at a time (read again from `next`): who is signed in, full mode, whether the camera can sign in, the lock times, and each profile's sign-in, PIN and face (set or not), wrong tries, capabilities (bits in `profile_cap_t`'s order) and apps (one digit per slot, the slot a `ShellApp` value, 31 Settings: 0 hidden, 1 read-only, 2 full). Never a PIN or its hash. A change replies `{"reply":"ok"}` or `{"error":...}`, with a trusted computer's rights but the model's rules: a manager needs a PIN, full mode a face and a PIN (enrolled on the board), one manager stays. `switch` cannot enter full mode: that takes the face at the board. |
| 54 · 61 | `{"op":"set_pin","id":N,"pin":"135790"}` / `{"op":"unblock","id":N}` / `{"op":"off"}` | Recovery for a forgotten PIN, over USB only (refused over the network, even from a paired computer): a new PIN for a profile (its wrong tries forgotten), the wrong tries forgotten, or every profile and face gone, so the board behaves as before profiles. Anyone with the board on USB can do this, as they could reinstall the firmware. |

While the USB webcam is on, ESP-Iris is stopped: read 44–46 after it ends.

Profiles (`components/profiles`) gate the apps and Swifty's tools through
`profiles_can()` and `profiles_app_access()`; with none set up the board
behaves as it always has. A forgotten PIN is reset with RPC 54 · 61 over USB,
for example `python mosaico.py iris rpc --project projects/glimdock 18244 54
--payload-hex` with `3d` (61) followed by the JSON's bytes. The model's host
tests run with
`cmake -S components/profiles/test -B build-host/profiles-test && cmake --build build-host/profiles-test && build-host/profiles-test/profiles-test`.

The VPN's settings and its switch are the `vpn` section of the configuration
(RPC 40, which `tools/mosaico-config.py` drives: `{"vpn": {"on": true}}`
connects), and `--status` shows its state, last handshake, traffic and errors
in words. RPC 52 shows where traffic goes: send a URL (`http://` or
`https://`, as hex) to start a GET of it, then call it with no payload for the
HTTP status, the default route and DNS servers in use and the start of the
body; a what's-my-IP address gives the public address the board leaves from.
Never put a real WireGuard key in a log, a screenshot, an issue or the
repository.

The VPN needs `CONFIG_ESP_NETIF_BRIDGE_EN=y` (in `sdkconfig.defaults`; see
the note there). A build directory made before it keeps its own
`build/sdkconfig`: delete that file, or set the option in menuconfig, or the
VPN refuses to start.

## Releasing to Mosaico Ideas

`mosaico-ideas.json` describes the store listing (category 27, Smart Home);
its covers, icon, website text and the Chinese font subset live in `store/`.
From the workspace root, with `libraqm` installed (`brew install libraqm` or
your distribution's `libraqm0`) and a Python environment holding
`submodule/esp-mosaico-utils/mosaico-tools/requirements-ideas.txt` and
`submodule/esp-mosaico-utils/ESP-Iris/components/esp_iris/tools/requirements.txt`:

```sh
# 1. Bump the version in CMakeLists.txt (project(glimdock VERSION ...)) and in
#    sdkconfig.defaults (CONFIG_APP_PROJECT_VER, which the build uses; an
#    existing build/sdkconfig keeps its own copy), then build the bundle
idf.py -C projects/glimdock -D ESP_IRIS_PYTHON="$(which python)" system-update-bundle
# 2. Package and check it (offline)
python mosaico.py project package --project projects/glimdock --output projects/glimdock/dist/glimdock-VERSION.zip
python mosaico.py project validate projects/glimdock/dist/glimdock-VERSION.zip --json
# 3. Upload as a draft (first time --create; later --update PUBLIC_ID)
python mosaico.py account login
python mosaico.py project upload projects/glimdock/dist/glimdock-VERSION.zip --create --yes --json
```

Covers, GIFs and the tour video come from the desktop preview's demo data,
never from a real board: those would show your own hosts, rooms and photos.
Regenerate them after UI changes (Pillow, ffmpeg and Chrome needed):

```sh
MOSAICO_PREVIEW_RECORD=1 GLIMDOCK_DIR=... python3 projects/glimdock/host/render.py \
    --output projects/glimdock/build-host/media-raw --home-fixtures projects/glimdock/host/fixtures/swifty-demo
python3 projects/glimdock/host/media.py projects/glimdock/build-host/media-raw
```

The store takes still covers only (no animated GIFs); the video goes in as an
attachment. If the Chinese
description changes, regenerate `store/fonts/NotoSansSC-Regular-subset.ttf`
with `pyftsubset` for its characters (the font is OFL; keep
`store/fonts/NotoSansSC-OFL.txt`).
