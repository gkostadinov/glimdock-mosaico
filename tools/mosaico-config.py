#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Configure the ESP-Mosaico's apps (Swifty, Glimdock, Home, Files, the display, the VPN) from one file.

  python3 projects/glimdock/tools/mosaico-config.py --config-file FILE
  python3 projects/glimdock/tools/mosaico-config.py --status
  python3 projects/glimdock/tools/mosaico-config.py --template > mosaico.json

FILE is JSON with any of six sections; leave out what you don't want to
change. Within a section a missing key keeps the board's value and (for
Swifty) an empty string clears it.

  "swifty":   llm_url, llm_key, llm_model, asr_url, asr_key, asr_model, asr_ca,
              search_key (Tavily, for Swifty's web search)
              Applies at once.
  "glimdock": endpoint, token, setup_token, ca, ssid, password,
              private_reader_file
              Saved as one set (validated as Glimdock's setup portal does: an
              https:// endpoint needs its CA); the board then restarts to apply
              it. Without ssid, Vibe Mode's Wi-Fi is used. private_reader_file
              (a path, ~ allowed, mode 0600) loads the private chat reader after the
              restart, as tools/private-reader-mosaico.py does.
  "home":     hub, pairing_code
              Pairs with Swifty Home using a one-time display code
              (Swifty Home: Displays, Pair a display). This tool waits for the
              result.
  "files":    shares: a list of SMB shares for the Files app, each with
              name, host (an IP address or a name the board can resolve),
              share, user, password and an optional start path. The list
              replaces the board's; an empty list removes them all. A
              share without "password" keeps the one saved for it (same
              name, or else the same host and share).
              photos: where the camera keeps its photos, {"share": a
              share's name, "path": a folder in it} (default: the first
              share, "Mosaico Photos").
              swifty: Swifty's own folder, the only place it writes (notes,
              lists, schedules), {"share", "path"} (default: the photos
              share, "Swifty").
  "display":  always_on: seconds without a touch or the AI key before the
              always-on display (a dim clock on black) comes up: 0 (off),
              30, 60, 120 (the default), 300 or 600. Applies at once.
  "vpn":      the VPN app's WireGuard tunnel. conf_file: a WireGuard .conf
              (a path, ~ allowed, owned by you, mode 0600) that replaces the
              whole configuration; or the fields private_key, address, dns,
              public_key (the server's), preshared_key, endpoint (host:port),
              allowed_ips (empty: 0.0.0.0/0, everything) and
              persistent_keepalive (seconds); auto_connect (true/false:
              connect at start-up); on (true/false: connect or disconnect
              now). Applies at once; --status shows the state and the board's
              public key, never a key.

The file must be an absolute path, owned by you, with mode 0600. Its contents
never appear on a command line or in a log: they reach the Mosaico CLI
in-process (service 0x4744, method 40). MOSAICO_PROJECT names another
checkout of the project (a worktree) whose ESP-Iris session to use. --status prints addresses and which
keys are set, never the keys.
"""

import argparse
import base64
import contextlib
import importlib.util
import io
import json
import os
import stat
import struct
import sys
import time
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[3]
# MOSAICO_PROJECT: another checkout (a worktree) whose ESP-Iris session to use;
# two projects' sessions compete for the board's one USB link.
PROJECT = Path(os.environ.get("MOSAICO_PROJECT") or WORKSPACE / "projects/glimdock").resolve()
SERVICE = 0x4744
METHOD = 40
CHUNK = 3000
SECTIONS = {
    "swifty": ("llm_url", "llm_key", "llm_model", "asr_url", "asr_key", "asr_model", "asr_ca", "search_key"),
    "glimdock": ("endpoint", "token", "setup_token", "ca", "ssid", "password", "private_reader_file"),
    "home": ("hub", "pairing_code"),
    "files": ("shares", "photos", "swifty"),
    "display": ("always_on",),
    "vpn": (
        "conf_file",
        "private_key",
        "address",
        "dns",
        "public_key",
        "preshared_key",
        "endpoint",
        "allowed_ips",
        "persistent_keepalive",
        "auto_connect",
        "on",
    ),
}
ALWAYS_ON = (0, 30, 60, 120, 300, 600)
SHARE_KEYS = ("name", "host", "share", "user", "password", "path")
TEMPLATE = {
    "swifty": {
        "llm_url": "http://MODEL-SERVER:8080/v1",
        "llm_key": "MODEL-SERVER-KEY",
        "llm_model": "",
        "asr_url": "wss://SPEECH-SERVER/v1/asr/realtime",
        "asr_key": "MOSAICO-ASR-KEY",
        "asr_model": "swifty-speech",
        "search_key": "TAVILY-KEY",
    },
    "glimdock": {
        "endpoint": "https://COLLECTOR-HOST/api/v1/snapshot",
        "token": "DISPLAY-TOKEN",
        "setup_token": "SETUP-TOKEN",
        "ca": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n",
        "private_reader_file": "~/.config/glimdock/private-reader.json",
    },
    "home": {
        "hub": "https://HOME-HUB",
        "pairing_code": "ABCD-1234",
    },
    "files": {
        "shares": [
            {
                "name": "Share",
                "host": "NAS-IP-ADDRESS",
                "share": "share",
                "user": "NAS-USER",
                "password": "NAS-PASSWORD",  # pragma: allowlist secret (a placeholder)
                "path": "",
            },
            {
                "name": "Media",
                "host": "NAS-IP-ADDRESS",
                "share": "media",
                "user": "NAS-USER",
                "password": "NAS-PASSWORD",  # pragma: allowlist secret (a placeholder)
                "path": "",
            },
        ],
        "photos": {"share": "Media", "path": "Photos"},
    },
    "display": {"always_on": 120},
    "vpn": {"conf_file": "~/.config/mosaico/wireguard.conf", "auto_connect": False, "on": True},
}


def mosaico():
    sys.path.insert(0, str(WORKSPACE / "submodule/esp-mosaico-utils/mosaico-tools/tools"))
    from mosaico_cli.cli import main  # noqa: E402

    return main


def cli_run(main, argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = main(argv, tool_root=WORKSPACE / "submodule/esp-mosaico-utils/mosaico-tools")
    return code, out.getvalue()


def rpc(main, payload: bytes) -> bytes:
    code, out = cli_run(
        main,
        ["--json", "iris", "rpc", "--project", str(PROJECT), str(SERVICE), str(METHOD), "--payload-hex", payload.hex()],
    )
    if code != 0:
        raise RuntimeError("the board did not answer")
    return base64.b64decode(json.loads(out)["response"].get("payload_base64") or "")


def status(main) -> dict:
    # A page at a time (op 5): the whole status is more than one reply holds.
    # Firmware without op 5 answers "unknown operation": one reply (op 0).
    text = b""
    while True:
        page = rpc(main, b"\5" + struct.pack("<I", len(text)))
        if not text and page == b"unknown operation":
            return json.loads(rpc(main, b"\0"))
        if not page:
            return json.loads(text)
        text += page


def check_private(path: Path, what: str) -> None:
    if not path.is_absolute():
        raise ValueError(f"{what}: use an absolute path")
    st = path.lstat()
    if not stat.S_ISREG(st.st_mode):
        raise ValueError(f"{what}: not a regular file")
    if st.st_uid != os.getuid():
        raise ValueError(f"{what}: the file must be owned by you")
    if stat.S_IMODE(st.st_mode) & 0o077:
        raise ValueError(f"{what}: the file must have mode 0600")


def load(path: str) -> dict:
    p = Path(path)
    check_private(p, "config file")
    data = json.loads(p.read_text())
    if not isinstance(data, dict) or not data:
        raise ValueError("expected a JSON object with swifty, glimdock and/or home")
    for name, section in data.items():
        if name not in SECTIONS:
            raise ValueError(f'unknown section "{name}" (use {", ".join(SECTIONS)})')
        if not isinstance(section, dict):
            raise ValueError(f"{name}: expected an object")
        unknown = sorted(set(section) - set(SECTIONS[name]))
        if unknown:
            raise ValueError(f"{name}: unknown keys {', '.join(unknown)}")
        if name == "files":
            shares = section.get("shares", [])
            if not isinstance(shares, list) or any(not isinstance(x, dict) for x in shares):
                raise ValueError("files.shares: expected a list of objects")
            for x in shares:
                bad = sorted(set(x) - set(SHARE_KEYS))
                if bad:
                    raise ValueError(f"files.shares: unknown keys {', '.join(bad)}")
                if any(not isinstance(v, str) for v in x.values()) or not x.get("host") or not x.get("share"):
                    raise ValueError("files.shares: each share needs host and share, all values strings")
            for key in ("photos", "swifty"):
                folder = section.get(key, {})
                if (
                    not isinstance(folder, dict)
                    or set(folder) - {"share", "path"}
                    or any(not isinstance(v, str) for v in folder.values())
                ):
                    raise ValueError(f'files.{key}: expected {{"share": "...", "path": "..."}}')
        elif name == "display":
            v = section.get("always_on", 0)
            if isinstance(v, bool) or v not in ALWAYS_ON:
                raise ValueError(f"display.always_on: seconds, one of {', '.join(map(str, ALWAYS_ON))} (0: off)")
        elif name == "vpn":
            for key, v in section.items():
                if key in ("auto_connect", "on"):
                    if not isinstance(v, bool):
                        raise ValueError(f"vpn.{key}: true or false")
                elif key == "persistent_keepalive":
                    if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 65535:
                        raise ValueError("vpn.persistent_keepalive: seconds from 0 (off) to 65535")
                elif not isinstance(v, str):
                    raise ValueError(f"vpn.{key}: expected a string")
            conf = section.get("conf_file")
            if conf:
                path = Path(conf).expanduser()
                check_private(path, "vpn.conf_file")
                section["conf"] = path.read_text()
                del section["conf_file"]
        elif any(not isinstance(v, str) for v in section.values()):
            raise ValueError(f"{name}: values must be strings")
    reader = data.get("glimdock", {}).get("private_reader_file")
    if reader:
        data["glimdock"]["private_reader_file"] = reader = str(Path(reader).expanduser())
        check_private(Path(reader), "glimdock.private_reader_file")
    return data


def send(main, config: dict) -> str:
    frame = bytearray(json.dumps(config, separators=(",", ":")).encode())
    try:
        reply = rpc(main, b"\1" + struct.pack("<I", len(frame)))
        for start in range(0, len(frame), CHUNK):
            if reply != b"ok":
                break
            reply = rpc(main, b"\2" + bytes(frame[start : start + CHUNK]))
        if reply == b"ok":
            reply = rpc(main, b"\3")
    except Exception:  # noqa: BLE001
        with contextlib.suppress(Exception):
            rpc(main, b"\4")
        raise
    finally:
        for i in range(len(frame)):
            frame[i] = 0
    return reply.decode(errors="replace")


def wait_for_board(main, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        with contextlib.suppress(Exception):
            status(main)
            return True
        time.sleep(2)
    return False


def follow_pairing(main) -> bool:
    deadline = time.monotonic() + 45
    note = "pairing"
    time.sleep(1.5)  # the board starts the pairing on its UI task
    while time.monotonic() < deadline:
        with contextlib.suppress(Exception):
            note = status(main).get("home", {}).get("pairing", "")
            if note and note != "pairing":
                break
        time.sleep(1.5)
    ok = note.startswith("paired")
    print(
        f"Home: {note or 'no answer'}" if ok else f"Home not paired: {note.removeprefix('failed: ') or 'no answer'}",
        file=sys.stdout if ok else sys.stderr,
    )
    return ok


def private_reader(path: str) -> bool:
    spec = importlib.util.spec_from_file_location(
        "private_reader_mosaico", Path(__file__).with_name("private-reader-mosaico.py")
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for attempt in range(6):  # Glimdock starts a little after the board
        try:
            module.provision(path, module.DEFAULT_TOOLS)
            print("Glimdock private reader provisioned.")
            return True
        except Exception:  # noqa: BLE001
            time.sleep(5)
    print("Glimdock private reader provisioning failed.", file=sys.stderr)
    return False


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--config-file", metavar="FILE")
    group.add_argument("--status", action="store_true")
    group.add_argument("--template", action="store_true", help="print a config to fill in")
    args = parser.parse_args(argv)
    if args.template:
        print(json.dumps(TEMPLATE, indent=2))
        return 0
    cli = mosaico()
    if args.status:
        try:
            print(json.dumps(status(cli), indent=2))
        except Exception as error:  # noqa: BLE001
            print(f"Status unavailable: {error}", file=sys.stderr)
            return 1
        return 0
    try:
        config = load(args.config_file)
    except (OSError, ValueError) as error:
        print(f"Configuration not loaded: {error}", file=sys.stderr)
        return 1
    reader = config.get("glimdock", {}).pop("private_reader_file", "")
    if "glimdock" in config and not config["glimdock"]:
        del config["glimdock"]
    ok = True
    if config:
        try:
            reply = send(cli, config)
        except Exception as error:  # noqa: BLE001
            print(f"Configuration not loaded: {error}", file=sys.stderr)
            return 1
        if not reply.startswith("ok"):
            print(f"Configuration not loaded: {reply}", file=sys.stderr)
            return 1
        for name in ("swifty", "glimdock", "files"):
            if name in config:
                print(f"{name.capitalize()} settings saved.")
        if "vpn" in config:
            print("VPN settings saved.")
        if "home" in config:
            ok = follow_pairing(cli) and ok
        if reply == "ok restart":
            print("Restarting the board so Glimdock uses its new settings...")
            code, _ = cli_run(cli, ["iris", "restart", "--project", str(PROJECT)])
            if code != 0 or not wait_for_board(cli, 90):
                print("The board did not come back after the restart; check it.", file=sys.stderr)
                return 1
    if reader:
        ok = private_reader(reader) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
