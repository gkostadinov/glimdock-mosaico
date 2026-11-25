#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Provision Glimdock's private reader on an ESP-Mosaico over ESP-Iris.

On the desk boards the reader is provisioned over a USB serial port
(Glimdock's tools/provision-private-reader.py). On the Mosaico, USB belongs
to ESP-Iris, so this sends the same frame through Glimdock's serial RPC
(service 0x4744: method 1 writes, method 2 reads the reply).

  python3 projects/glimdock/tools/private-reader-mosaico.py --credentials-file FILE

The credentials file is checked exactly as by the USB tool (absolute path, owned
by you, mode 0600). Its contents never appear on a command line or in a log:
the frame is passed to the Mosaico CLI in-process and on to ESP-Iris by stdin.
Only success or failure is printed.
"""
import argparse
import base64
import contextlib
import importlib.util
import io
import json
import os
import secrets
import sys
import time
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[3]
# Glimdock's own provisioning tool: GLIMDOCK_DIR names a Glimdock checkout
# (https://github.com/gkostadinov/glimdock); --homelab-tools overrides it.
DEFAULT_TOOLS = Path(os.environ.get("GLIMDOCK_DIR", ".")) / "tools"
SERVICE = 0x4744
CHUNK = 3000            # below the 4000-byte ESP-Iris RPC body
ACK_TIMEOUT_S = 20.0
SUCCESS = "Private reader provisioned."
FAILURE = "Private reader provisioning failed."


def load_usb_tool(tools: Path):
    spec = importlib.util.spec_from_file_location("provision_private_reader", tools / "provision-private-reader.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def mosaico():
    sys.path.insert(0, str(WORKSPACE / "submodule/esp-mosaico-utils/mosaico-tools/tools"))
    from mosaico_cli.cli import main  # noqa: E402
    return main


def rpc(main, method: int, payload: bytes = b"") -> bytes:
    """One RPC through the Mosaico CLI, in-process; returns the reply bytes."""
    argv = ["--json", "iris", "rpc", "--project", str(WORKSPACE / "projects/glimdock"), str(SERVICE), str(method)]
    if payload:
        argv += ["--payload-hex", payload.hex()]
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = main(argv, tool_root=WORKSPACE / "submodule/esp-mosaico-utils/mosaico-tools")
    if code != 0:
        raise RuntimeError("rpc failed")
    reply = json.loads(out.getvalue())
    return base64.b64decode(reply["response"].get("payload_base64") or "")


def provision(credentials_file: str, tools: Path) -> None:
    usb = load_usb_tool(tools)
    credentials = usb.load_credentials(credentials_file)
    nonce = secrets.token_urlsafe(24)
    frame = bytearray(usb.build_frame(credentials, nonce))
    main = mosaico()
    try:
        rpc(main, 2)  # drop any stale reply
        for start in range(0, len(frame), CHUNK):
            chunk = bytes(frame[start:start + CHUNK])
            accepted = int.from_bytes(rpc(main, 1, chunk)[:4] or b"\0", "little")
            if accepted != len(chunk):
                raise RuntimeError("frame not accepted")
    finally:
        for i in range(len(frame)):
            frame[i] = 0
    ok = usb.ACK_OK_PREFIX + nonce.encode("ascii")
    error = usb.ACK_ERROR_PREFIX + nonce.encode("ascii")
    pending = b""
    deadline = time.monotonic() + ACK_TIMEOUT_S
    while time.monotonic() < deadline:
        pending += rpc(main, 2)
        *lines, pending = pending.split(b"\n")
        for line in lines:
            line = line.rstrip(b"\r")
            if line == ok:
                return
            if line == error:
                raise RuntimeError("rejected")
        time.sleep(0.4)
    raise RuntimeError("no reply")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--credentials-file", required=True, metavar="FILE")
    parser.add_argument("--homelab-tools", type=Path, default=DEFAULT_TOOLS,
                        help="Glimdock's tools directory (default: $GLIMDOCK_DIR/tools; for the shared validation and frame format)")
    args = parser.parse_args(argv)
    try:
        provision(args.credentials_file, args.homelab_tools)
    except (Exception, KeyboardInterrupt):
        print(FAILURE, file=sys.stderr)
        return 1
    print(SUCCESS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
