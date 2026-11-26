#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Download the Camera app's photos from an ESP-Mosaico to this computer.

Photos live in the board's PSRAM until it restarts. This copies them over
ESP-Iris (service 0x4744: method 20 lists, method 21 reads) into a folder:

  python3 projects/glimdock/tools/camera-photos.py [--to ~/Pictures/Mosaico]

Files are named by the time they were taken (mosaico-YYYYMMDD-HHMMSS.jpg) and
existing files are left alone, so running it again only fetches new photos.
"""

import argparse
import base64
import contextlib
import io
import json
import struct
import sys
import time
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[3]
SERVICE = 0x4744
CHUNK = 3800


def cli():
    sys.path.insert(0, str(WORKSPACE / "submodule/esp-mosaico-utils/mosaico-tools/tools"))
    from mosaico_cli.cli import main  # noqa: E402

    return main


def rpc(main, method: int, payload: bytes = b"") -> bytes:
    argv = ["--json", "iris", "rpc", "--project", str(WORKSPACE / "projects/glimdock"), str(SERVICE), str(method)]
    if payload:
        argv += ["--payload-hex", payload.hex()]
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = main(argv, tool_root=WORKSPACE / "submodule/esp-mosaico-utils/mosaico-tools")
    reply = json.loads(out.getvalue() or "{}")
    if code != 0 or not reply.get("ok"):
        raise SystemExit("The board did not answer; is the Camera app opened once since it started?")
    return base64.b64decode(reply["response"].get("payload_base64") or "")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--to", type=Path, default=Path.home() / "Pictures/Mosaico")
    args = parser.parse_args(argv)
    main_cli = cli()
    listing = rpc(main_cli, 20)
    count = struct.unpack_from("<H", listing, 0)[0]
    args.to.mkdir(parents=True, exist_ok=True)
    saved = 0
    for index in range(count):
        size, taken = struct.unpack_from("<II", listing, 2 + 8 * index)
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(taken)) if taken else f"photo-{index + 1:02d}"
        path = args.to / f"mosaico-{stamp}.jpg"
        if path.exists() and path.stat().st_size == size:
            continue
        data = bytearray()
        while len(data) < size:
            data += rpc(main_cli, 21, struct.pack("<HI", index, len(data)))
        path.write_bytes(bytes(data[:size]))
        saved += 1
        print(f"{path.name}  {size // 1024} KB")
    print(f"{saved} new of {count} photo{'s' if count != 1 else ''} in {args.to}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
