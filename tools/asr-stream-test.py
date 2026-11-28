#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stream speech to the realtime ASR endpoint as Swifty's firmware does, to
find the server's limits (session length, idle time, upstream failures).

  python3 projects/glimdock/tools/asr-stream-test.py --config-file FILE --seconds 5 15 30 60
  python3 projects/glimdock/tools/asr-stream-test.py --config-file FILE --seconds 20 --warm 45
  python3 projects/glimdock/tools/asr-stream-test.py --config-file FILE --seconds 90 --reconnect 4
  python3 projects/glimdock/tools/asr-stream-test.py --mock --seconds 5 70

FILE is the board configuration tools/mosaico-config.py reads (absolute path,
owned by you, mode 0600); its "swifty" section gives asr_url, asr_key,
asr_model and asr_ca. The key is only sent to that URL, never printed.

Each run: connect, wait --warm seconds (the firmware keeps a connection
ready while Swifty is open), run-task, then 16 kHz mono PCM in 80 ms chunks
at the speaking pace, finish-task, and the final words. The clip comes from
--pcm (raw s16le, 16 kHz, mono; looped to the length) or is spoken by macOS
`say` and converted with ffmpeg. --reconnect N makes the connection again
when it drops and sends the audio since the last final sentence once more,
as the firmware does. --mock runs against a local server that ends sessions
after 60 s, to try the tool itself.

Only the Python standard library is used (a minimal WebSocket client).
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path
from urllib.parse import urlparse

RATE = 16000
CHUNK = RATE * 2 * 80 // 1000  # 80 ms of s16le mono
TEXT = (
    "This is a long dictation for the speech test. It keeps talking about ordinary things, the weather, "
    "the garden, a list of groceries and the plans for the weekend, so the recogniser has words to find. "
)


# ---- a minimal WebSocket client (RFC 6455) ----------------------------------------------------
class Socket:
    def __init__(self, url: str, headers: dict[str, str], ca: str | None, timeout: float = 15):
        u = urlparse(url)
        secure = u.scheme == "wss"
        port = u.port or (443 if secure else 80)
        raw = socket.create_connection((u.hostname, port), timeout=timeout)
        if secure:
            ctx = ssl.create_default_context(cadata=ca) if ca else ssl.create_default_context()
            raw = ctx.wrap_socket(raw, server_hostname=u.hostname)
        self.sock = raw
        self.lock = threading.Lock()
        key = base64.b64encode(os.urandom(16)).decode()
        path = (u.path or "/") + (f"?{u.query}" if u.query else "")
        lines = [
            f"GET {path} HTTP/1.1",
            f"Host: {u.hostname}:{port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
            "User-Agent: swifty-mosaico-test",
        ]
        lines += [f"{k}: {v}" for k, v in headers.items()]
        raw.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        reply = b""
        while b"\r\n\r\n" not in reply:
            data = raw.recv(4096)
            if not data:
                raise ConnectionError("closed during the upgrade")
            reply += data
        head, self.buffer = reply.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0].decode(errors="replace")
        if " 101 " not in status + " ":
            raise ConnectionError(f"upgrade refused: {status}")
        raw.settimeout(None)

    def send(self, opcode: int, payload: bytes) -> None:
        header = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack("!H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", n)
        mask = os.urandom(4)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        with self.lock:
            self.sock.sendall(header + mask + body)

    def _read(self, n: int) -> bytes:
        while len(self.buffer) < n:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("connection closed (no close frame)")
            self.buffer += data
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def recv(self) -> tuple[int, bytes]:
        """One message: (opcode, payload). Answers pings itself."""
        while True:
            b0, b1 = self._read(2)
            opcode, n = b0 & 15, b1 & 127
            if n == 126:
                n = struct.unpack("!H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else b""
            payload = self._read(n)
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == 9:
                self.send(10, payload)
                continue
            if opcode == 10:
                continue
            return opcode, payload

    def close(self) -> None:
        try:
            self.send(8, struct.pack("!H", 1000))
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# ---- the firmware's protocol ----------------------------------------------------------------
def message(action: str, task: str, model: str | None = None) -> str:
    doc = {"header": {"action": action, "task_id": task, "streaming": "duplex"}, "payload": {"input": {}}}
    if action == "run-task":
        doc["payload"].update(
            {
                "task_group": "audio",
                "task": "asr",
                "function": "recognition",
                "model": model,
                "parameters": {"format": "pcm", "sample_rate": RATE},
            }
        )
    return json.dumps(doc)


def run(
    url: str, key: str, model: str, ca: str | None, pcm: bytes, warm: float, show: bool, reconnects: int = 0
) -> dict:
    """One dictation. With reconnects, a connection that drops is made again and
    the audio since the last final sentence sent once more, as the firmware does."""
    t0 = time.monotonic()
    stamp = lambda: f"{time.monotonic() - t0:7.2f} s"  # noqa: E731
    bps = RATE * 2
    out = {"seconds": len(pcm) / bps, "warm": warm, "ok": False, "sent": 0.0, "words": "", "why": "", "reconnects": 0}
    committed: list[str] = []
    st = {"sent": 0, "task_from": 0, "commit_at": 0, "partial": ""}
    press = None
    for attempt in range(reconnects + 1):
        try:
            ws = Socket(url, {"Authorization": f"Bearer {key}"} if key else {}, ca)
        except (OSError, ConnectionError, ssl.SSLError) as e:
            out["why"] = f"connect: {e}"
            break
        print(f"  {stamp()} connected")
        events: list[str] = []
        closed = {"why": ""}
        started, finished = threading.Event(), threading.Event()

        def reader(
            ws: Socket = ws,
            events: list = events,
            closed: dict = closed,
            started: threading.Event = started,
            finished: threading.Event = finished,
        ) -> None:
            try:
                while True:
                    op, data = ws.recv()
                    if op == 8:
                        code = struct.unpack("!H", data[:2])[0] if len(data) >= 2 else 0
                        closed["why"] = f"server closed (code {code} {data[2:].decode(errors='replace')})".strip()
                        break
                    if op != 1:
                        continue
                    doc = json.loads(data)
                    event = doc.get("header", {}).get("event", "")
                    events.append(event)
                    if event == "task-started":
                        print(f"  {stamp()} task-started")
                        started.set()
                    elif event == "result-generated":
                        sen = doc.get("payload", {}).get("output", {}).get("sentence", {}) or {}
                        if sen.get("heartbeat"):
                            continue
                        if sen.get("sentence_end"):
                            committed.append(sen.get("text", ""))
                            st["partial"] = ""
                            # Where these words end in the audio (as the firmware): end_time
                            # when given, else what was sent by now.
                            at, ms = st["sent"], sen.get("end_time")
                            if isinstance(ms, int) and ms > 0:
                                at = min(at, st["task_from"] + ms * 32 // 640 * 640)
                            st["commit_at"] = max(st["commit_at"], at)
                            print(
                                f"  {stamp()} sentence {sen.get('sentence_id')} final ({len(sen.get('text', ''))} chars, "
                                f"begin {sen.get('begin_time')} end {ms} ms)"
                            )
                        else:
                            st["partial"] = sen.get("text", "")
                    elif event in ("task-finished", "task-failed"):
                        h = doc.get("header", {})
                        print(f"  {stamp()} {event} {h.get('error_code', '')} {h.get('error_message', '')}".rstrip())
                        if event == "task-failed":
                            closed["why"] = (
                                f"task-failed: {h.get('error_code', '')} {h.get('error_message', '')}".strip()
                            )
                        finished.set()
            except (OSError, ConnectionError, ValueError) as e:
                closed["why"] = closed["why"] or str(e)
            finished.set()
            started.set()

        threading.Thread(target=reader, daemon=True).start()
        if warm and attempt == 0:
            print(f"  {stamp()} waiting {warm:g} s with the connection open")
            finished.wait(warm)
            if finished.is_set():
                out["why"] = f"dropped while waiting, {time.monotonic() - t0:.1f} s after connecting: {closed['why']}"
                ws.close()
                break
        task = uuid.uuid4().hex
        ws.send(1, message("run-task", task, model).encode())
        if not started.wait(10) or finished.is_set():
            out["why"] = closed["why"] or "no task-started in 10 s"
            ws.close()
            break
        press = press or time.monotonic()  # the speaking pace counts from the first task
        try:
            while st["sent"] < len(pcm) and not finished.is_set():
                i = st["sent"]
                ahead = press + i / bps - time.monotonic()
                if ahead > 0:
                    time.sleep(ahead)
                ws.send(2, pcm[i : i + CHUNK])
                st["sent"] = min(len(pcm), i + CHUNK)
                out["sent"] = max(out["sent"], st["sent"] / bps)
            if not finished.is_set():
                print(f"  {stamp()} audio sent ({out['sent']:.1f} s), finish-task")
                ws.send(1, message("finish-task", task).encode())
        except OSError as e:
            closed["why"] = closed["why"] or f"send: {e}"
        finished.wait(30)
        ws.close()
        if "task-finished" in events and st["sent"] >= len(pcm):
            out["ok"] = True
            break
        out["why"] = closed["why"] or (
            "no task-finished in 30 s" if "task-finished" not in events else "the task finished early"
        )
        out["why"] += f" ({st['sent'] / bps:.1f} s of audio sent, {time.monotonic() - t0:.1f} s after the start)"
        if attempt < reconnects:
            st["sent"] = st["task_from"] = st["commit_at"]
            out["reconnects"] += 1
            print(f"  {stamp()} {out['why']}: connecting again, from {st['sent'] / bps:.1f} s of audio")
    out["words"] = "".join(committed) + st["partial"]
    if out["ok"]:
        out["why"] = f"{out['reconnects']} reconnect(s)" if out["reconnects"] else ""
    if show and out["words"]:
        print("  words:", out["words"])
    return out


# ---- audio ----------------------------------------------------------------------------------
def speech(seconds: float, pcm_file: str | None) -> bytes:
    if pcm_file:
        clip = Path(pcm_file).read_bytes()
    else:
        with tempfile.TemporaryDirectory() as d:
            aiff, raw = Path(d) / "x.aiff", Path(d) / "x.pcm"
            subprocess.run(["say", "-o", str(aiff), TEXT * 3], check=True)
            subprocess.run(
                [
                    "ffmpeg",
                    "-loglevel",
                    "error",
                    "-y",
                    "-i",
                    str(aiff),
                    "-ar",
                    str(RATE),
                    "-ac",
                    "1",
                    "-f",
                    "s16le",
                    str(raw),
                ],
                check=True,
            )
            clip = raw.read_bytes()
    need = int(seconds * RATE) * 2
    return (clip * (need // max(1, len(clip)) + 1))[:need]


# ---- a local server, to try the tool -------------------------------------------------------
def mock_server(limit: float) -> int:
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)

    def frame(opcode: int, payload: bytes) -> bytes:
        n = len(payload)
        head = bytes([0x80 | opcode]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack("!H", n))
        return head + payload

    def serve(c: socket.socket) -> None:
        req = b""
        while b"\r\n\r\n" not in req:
            req += c.recv(4096)
        key = next(
            line.split(b":", 1)[1].strip()
            for line in req.split(b"\r\n")
            if line.lower().startswith(b"sec-websocket-key")
        )
        import hashlib

        accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest()).decode()
        c.sendall(
            f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n".encode()
        )
        born, heard, sid = time.monotonic(), 0, 0
        c.settimeout(0.5)
        buf = b""
        task = ""
        while time.monotonic() - born < limit:
            try:
                data = c.recv(65536)
            except socket.timeout:
                continue
            if not data:
                return
            buf += data
            while len(buf) >= 2:
                n, off = buf[1] & 127, 2
                if n == 126:
                    if len(buf) < 4:
                        break
                    n, off = struct.unpack("!H", buf[2:4])[0], 4
                if len(buf) < off + 4 + n:
                    break
                mask, payload = buf[off : off + 4], buf[off + 4 : off + 4 + n]
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
                op, buf = buf[0] & 15, buf[off + 4 + n :]
                if op == 1:
                    doc = json.loads(payload)
                    task = doc["header"]["task_id"]
                    reply = "task-started" if doc["header"]["action"] == "run-task" else "task-finished"
                    c.sendall(frame(1, json.dumps({"header": {"event": reply, "task_id": task}}).encode()))
                elif op == 2:
                    heard += len(payload)
                    if heard >= RATE * 2 * 3:  # a "sentence" every 3 s of audio
                        heard = 0
                        doc = {
                            "header": {"event": "result-generated", "task_id": task},
                            "payload": {
                                "output": {
                                    "sentence": {"sentence_id": sid, "text": f"sentence {sid}. ", "sentence_end": True}
                                }
                            },
                        }
                        sid += 1
                        c.sendall(frame(1, json.dumps(doc).encode()))
                elif op == 8:
                    return
        c.sendall(frame(8, struct.pack("!H", 1000) + b"session limit"))
        c.close()

    def loop() -> None:
        while True:
            c, _ = srv.accept()
            threading.Thread(target=serve, args=(c,), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return srv.getsockname()[1]


def load_config(path: str) -> dict:
    p = Path(path)
    if not p.is_absolute():
        raise SystemExit("--config-file: use an absolute path")
    st = p.stat()
    if st.st_uid != os.getuid() or st.st_mode & 0o077:
        raise SystemExit("--config-file: must be yours with mode 0600")
    return json.loads(p.read_text()).get("swifty", {})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config-file", help="the board configuration (swifty.asr_url, asr_key, asr_model, asr_ca)")
    ap.add_argument("--mock", action="store_true", help="a local server that ends sessions after 60 s")
    ap.add_argument("--seconds", type=float, nargs="+", default=[5, 15, 30, 60], help="dictation lengths to try")
    ap.add_argument("--warm", type=float, default=0, help="seconds the connection waits open before the task")
    ap.add_argument("--pcm", help="a raw s16le 16 kHz mono clip instead of macOS speech")
    ap.add_argument("--show-text", action="store_true", help="print the words recognised")
    ap.add_argument(
        "--reconnect",
        type=int,
        default=0,
        metavar="N",
        help="when the connection drops, connect again (up to N times) from the last final sentence",
    )
    a = ap.parse_args()
    if a.mock:
        url, key, model, ca = f"ws://127.0.0.1:{mock_server(60)}/v1/asr/realtime", "", "swifty-speech", None
    elif a.config_file:
        cfg = load_config(a.config_file)
        url, key, model, ca = (
            cfg.get("asr_url", ""),
            cfg.get("asr_key", ""),
            cfg.get("asr_model") or "swifty-speech",
            cfg.get("asr_ca") or None,
        )
        if not url:
            raise SystemExit("no swifty.asr_url in the configuration")
    else:
        raise SystemExit("give --config-file FILE or --mock")
    results = []
    for seconds in a.seconds:
        print(f"{seconds:g} s of speech{f', after {a.warm:g} s waiting' if a.warm else ''}:")
        results.append(run(url, key, model, ca, speech(seconds, a.pcm), a.warm, a.show_text, a.reconnect))
        r = results[-1]
        print(f"  -> {'ok' if r['ok'] else 'FAILED: ' + r['why']}; {len(r['words'])} characters recognised")
    print("\nsummary:")
    for r in results:
        print(f"  {r['seconds']:5.0f} s  {'ok' if r['ok'] else 'failed'}  {r['why']}")
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
