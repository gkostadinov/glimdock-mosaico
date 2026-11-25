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
- **The FreeRTOS tick is 100 Hz**: `pdMS_TO_TICKS()` below 10 ms is 0 ticks, so
  a short `vTaskDelay` spins. Wait on notifications, or delay whole ticks.
- **Priorities:** tasks at or above LVGL's draw threads (priority 3) can starve
  the UI. Keep background work at 2 or below unless it blocks quickly.

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
| 10 | none, or `01 r` / `02 e` / `03 m` | Orientation: accelerometer, gravity, rotation; force a rotation, toggle auto-rotate or set the axis map. |
| 11 | `01` resets | Frame statistics: frames, render and flush times. |
| 13, 14 | none | Mark, then CPU per task since the mark. |
| 15 | seconds (starts) / none (result) | Time spent in every LVGL timer callback, by address (symbolise with `addr2line`). |
| 24 | none | Camera facing and why. |
| 30 | app (`00` launcher … `06` Tools, then a Tools tab) | Opens an app. |
| 41, 42 | text / `01`, `00` | Swifty says something / listens. |
| 43 | seconds, `01` (encode with no computer watching), rotation ÷ 90 + 1, `02` (YUV420) | A timed USB webcam session that ends by itself. |
| 44 | none | The last webcam session: USB setup, stream commits, frames, the latest control requests. |
| 45 | none | The last webcam session's effect on the board: LVGL loop gaps, CPU windows, time per frame by stage. |
| 46 | 32-bit offset (little-endian) | The last webcam JPEG, 960 bytes at a time. |

While the USB webcam is on, ESP-Iris is stopped: read 44–46 after it ends.
