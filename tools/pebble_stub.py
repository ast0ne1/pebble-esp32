#!/usr/bin/env python3
"""Pebble protocol v1 stub: stands in for StonePi AgentLink while the firmware is built.

Serves PROTOCOL.md (claim, config, mood, ask, unpair) with the same paths, errors and
limits as the real Pi. Standard library only, so it runs anywhere with Python 3.10+.

    python tools/pebble_stub.py                  # LAN :8014, away :8016, control page on :8014
                                                 # (8015 is real AgentLink on the Pi)
    python tools/pebble_stub.py --auto-claim 5   # claim any code 5 s after its first poll

Two listeners:
  LAN port   plays the Pi on the home network (plain HTTP), plus the control page at /
  away port  plays Tailscale Funnel. Plain HTTP here; the real one is HTTPS on :8443.
             Claim is refused and devices without "Away from home" get 403.

The control page stands in for Notify -> Sidekick: claim a code, rename a device, allow
away use, revoke, pin a mood, force the next ask to fail, or make the LAN port drop
connections to test host failover. It has no login: run it on a trusted network only.

Asks are answered with a chime followed by your own recording played back at 22.05 kHz,
so the round trip (resample, upload, playback) is audible.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import html
import io
import json
import math
import secrets
import socket
import sys
import threading
import time
import wave
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote

BASE = "/agent/pebble/v1"
CODE_ALPHABET = "ACDEFHJKMNPRTUVWXY34679"
CODE_TTL_S = 15 * 60
MOODS = ["happy", "content", "sleepy", "curious", "worried", "upset", "proud"]
MOOD_CAPTIONS = {
    "happy": "All good at home",
    "content": "",
    "sleepy": "Quiet day",
    "curious": "Something new on the pinboard",
    "worried": "Something needs a look",
    "upset": "StonePi needs help",
    "proud": "Glad I could help",
}
ASK_MAX_BYTES = 512 * 1024
DRAIN_MAX_BYTES = 4 * 1024 * 1024  # read oversized uploads up to this, so the 413 arrives
ASK_MAX_S = 15
ASK_RATE = 16000
REPLY_RATE = 22050
MOOD_MIN_INTERVAL_S = 10
CLAIM_MIN_INTERVAL_S = 1.0
FORCEABLE_ERRORS = {
    "busy": (409, "Pebble is already answering", 5),
    "no_speech": (422, "Didn't catch that", None),
    "too_large": (413, "That was too long", None),
    "owner_disabled": (403, "Account is disabled", None),
    "away_disabled": (403, "Away use is off for this Pebble", None),
    "ai_unavailable": (503, "AI isn't set up on StonePi", 60),
    "speech_unavailable": (503, "Speech isn't working right now", 60),
    "rate_limited": (429, "Slow down a little", 10),
    "internal": (500, "Something went wrong", None),
}


def now() -> float:
    return time.time()


def iso(ts: float) -> str:
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def header_text(text: str) -> str:
    raw = text.encode("utf-8")[:200].decode("utf-8", "ignore")
    return quote(raw, safe=" !'()*,-.:;?")


def lan_ip() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))  # no packet is sent; picks the outbound interface
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


class ApiError(Exception):
    def __init__(self, status: int, error: str, message: str, retry_after: int | None = None):
        super().__init__(message)
        self.status, self.error, self.message, self.retry_after = status, error, message, retry_after


# -- state ------------------------------------------------------------------------------


class Stub:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.lock = threading.RLock()
        self.ask_lock = threading.Lock()
        self.state_path = Path(args.state)
        self.devices: dict[str, dict] = {}
        self.pending: dict[str, dict] = {}
        self.used_codes: set[str] = set()
        self.last_claim_poll: dict[str, float] = {}
        self.last_mood_poll: dict[str, float] = {}
        self.mood_index = 0
        self.mood_since = now()
        self.pinned_mood = ""
        self.next_error = ""
        self.lan_down = False
        self.log: deque[str] = deque(maxlen=60)
        self._load()

    # persistence: devices only (pending codes and toggles are runtime)
    def _load(self) -> None:
        try:
            data = json.loads(self.state_path.read_text(encoding="utf-8"))
            self.devices = data.get("devices", {})
        except (OSError, ValueError):
            self.devices = {}

    def save(self) -> None:
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.state_path.with_suffix(".tmp")
        tmp.write_text(json.dumps({"devices": self.devices}, indent=2), encoding="utf-8")
        tmp.replace(self.state_path)

    def note(self, line: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S")
        self.log.appendleft(f"{stamp} {line}")
        print(f"{stamp} {line}", flush=True)

    # hosts as the Pi would advertise them
    def hosts(self, device: dict) -> dict:
        host = self.args.public_host
        away = f"http://{host}:{self.args.away_port}" if device.get("away_allowed") else None
        return {"lan": f"http://{host}:{self.args.lan_port}", "away": away}

    def config(self, device: dict) -> dict:
        return {
            "rev": device["rev"],
            "nickname": device["nickname"],
            "character": "Pebble",
            "owner": device["owner"],
            "hosts": self.hosts(device),
            "away_allowed": bool(device.get("away_allowed")),
            "talk": device["talk"],
            "mood_poll_s": 30,
            "max_ask_s": ASK_MAX_S,
        }

    def device_for(self, auth: str) -> tuple[str, dict]:
        if not auth.startswith("Bearer "):
            raise ApiError(401, "unauthorized", "Pair Pebble again")
        digest = token_hash(auth[7:].strip())
        for device_id, device in self.devices.items():
            if secrets.compare_digest(device["token_hash"], digest):
                return device_id, device
        raise ApiError(401, "unauthorized", "Pair Pebble again")

    def bump(self, device: dict) -> None:
        device["rev"] = int(device.get("rev", 0)) + 1


# -- WAV helpers --------------------------------------------------------------------------


def read_ask_wav(body: bytes) -> array.array:
    try:
        with wave.open(io.BytesIO(body), "rb") as w:
            if w.getcomptype() != "NONE" or w.getnchannels() != 1 or w.getsampwidth() != 2:
                raise ApiError(415, "unsupported_audio", "Send 16 kHz mono 16-bit WAV")
            if w.getframerate() != ASK_RATE:
                raise ApiError(415, "unsupported_audio", "Send 16 kHz mono 16-bit WAV")
            frames = w.readframes(w.getnframes())
    except (wave.Error, EOFError) as exc:
        raise ApiError(415, "unsupported_audio", "Send 16 kHz mono 16-bit WAV") from exc
    samples = array.array("h")
    samples.frombytes(frames[: len(frames) // 2 * 2])
    if sys.byteorder == "big":
        samples.byteswap()
    return samples


def chime() -> list[float]:
    out: list[float] = []
    for freq in (880.0, 1320.0):
        n = int(REPLY_RATE * 0.12)
        for i in range(n):
            fade = min(1.0, i / 200, (n - i) / 200)
            out.append(0.3 * fade * math.sin(2 * math.pi * freq * i / REPLY_RATE))
    out.extend([0.0] * int(REPLY_RATE * 0.15))
    return out


def reply_wav(samples: array.array) -> bytes:
    peak = max((abs(s) for s in samples), default=0) or 1
    gain = min(8.0, 24000 / peak)
    n_out = int(len(samples) * REPLY_RATE / ASK_RATE)
    step = ASK_RATE / REPLY_RATE
    echo: list[float] = []
    last = len(samples) - 1
    for i in range(n_out):
        pos = i * step
        j = int(pos)
        frac = pos - j
        a = samples[j]
        b = samples[min(j + 1, last)]
        echo.append((a + (b - a) * frac) * gain / 32768)
    pcm = array.array("h", (max(-32767, min(32767, int(v * 32767))) for v in chime() + echo))
    if sys.byteorder == "big":
        pcm.byteswap()
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(REPLY_RATE)
        w.writeframes(pcm.tobytes())
    return buf.getvalue()


def level_dbfs(samples: array.array) -> float:
    if not samples:
        return -120.0
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    return 20 * math.log10(max(rms, 1) / 32768)


# -- HTTP ---------------------------------------------------------------------------------


def make_handler(stub: Stub, kind: str):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "PebbleStub/1"

        def log_message(self, fmt, *args):  # quiet default logging; we log our own line
            pass

        # responses
        def _send(self, status: int, body: bytes, ctype: str, headers: dict | None = None) -> None:
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _json(self, status: int, payload: dict, headers: dict | None = None) -> None:
            self._send(status, json.dumps(payload).encode(), "application/json; charset=utf-8", headers)

        def _error(self, err: ApiError) -> None:
            payload = {"error": err.error, "message": err.message}
            headers = {}
            if err.retry_after:
                payload["retry_after_s"] = err.retry_after
                headers["Retry-After"] = str(err.retry_after)
            self._json(err.status, payload, headers)

        def _body(self, limit: int) -> bytes:
            if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
                out = bytearray()
                total = 0
                while True:
                    size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                    if size == 0:
                        self.rfile.readline()
                        break
                    chunk = self.rfile.read(size)
                    self.rfile.readline()
                    total += size
                    if total > DRAIN_MAX_BYTES:
                        self.close_connection = True
                        break
                    if total <= limit:
                        out += chunk
                if total > limit:
                    raise ApiError(413, "too_large", "That was too long")
                return bytes(out)
            length = int(self.headers.get("Content-Length") or 0)
            if length > limit:
                # Read the upload before answering, or the client sees a reset instead of 413.
                if length > DRAIN_MAX_BYTES:
                    self.close_connection = True
                else:
                    remaining = length
                    while remaining > 0:
                        got = self.rfile.read(min(65536, remaining))
                        if not got:
                            break
                        remaining -= len(got)
                raise ApiError(413, "too_large", "That was too long")
            return self.rfile.read(length) if length else b""

        # routing
        def do_GET(self):
            self._route("GET")

        def do_POST(self):
            self._route("POST")

        def _route(self, method: str) -> None:
            path = self.path.split("?", 1)[0]
            if kind == "lan" and not path.startswith(BASE) and method in {"GET", "POST"}:
                if path == "/" or path.startswith("/stub/"):
                    return self._control(method, path)
            if path.startswith(BASE) and kind == "lan" and stub.lan_down:
                stub.note(f"{kind:4} {method} {path} -> dropped (LAN down)")
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                return
            status = 500
            try:
                status = self._api(method, path[len(BASE):] if path.startswith(BASE) else None)
            except ApiError as err:
                status = err.status
                self._error(err)
            except Exception as exc:  # noqa: BLE001 - a stub should answer, not die
                stub.note(f"error: {exc!r}")
                self._error(ApiError(500, "internal", "Something went wrong"))
            stub.note(f"{kind:4} {method} {path} -> {status}")

        def _api(self, method: str, sub: str | None) -> int:
            if sub is None:
                raise ApiError(404, "not_found", "Not found")
            if method == "POST" and sub == "/claim/poll":
                return self._claim()
            with stub.lock:
                device_id, device = stub.device_for(self.headers.get("Authorization", ""))
                if kind == "away" and not device.get("away_allowed"):
                    raise ApiError(403, "away_disabled", "Away use is off for this Pebble")
                device["last_seen"] = now()
                device["last_path"] = kind
                device["fw"] = self.headers.get("X-Pebble-Fw", device.get("fw", ""))
                device["battery"] = self.headers.get("X-Pebble-Battery", "")
            if method == "GET" and sub == "/config":
                with stub.lock:
                    self._json(200, stub.config(device))
                return 200
            if method == "GET" and sub == "/mood":
                return self._mood(device_id, device)
            if method == "POST" and sub == "/ask":
                return self._ask(device)
            if method == "POST" and sub == "/unpair":
                self._body(4096)
                with stub.lock:
                    stub.devices.pop(device_id, None)
                    stub.save()
                self._send(204, b"", "text/plain")
                return 204
            raise ApiError(404, "not_found", "Not found")

        def _claim(self) -> int:
            if kind == "away":
                raise ApiError(404, "not_found", "Pair Pebble at home")
            ip = self.client_address[0]
            with stub.lock:
                last = stub.last_claim_poll.get(ip, 0)
                if now() - last < CLAIM_MIN_INTERVAL_S:
                    raise ApiError(429, "rate_limited", "Slow down a little", 1)
                stub.last_claim_poll[ip] = now()
            try:
                data = json.loads(self._body(4096) or b"{}")
            except ValueError as exc:
                raise ApiError(400, "bad_request", "Body must be JSON") from exc
            code = str(data.get("code", "")).upper()
            device_id = str(data.get("device_id", "")).lower()
            if len(code) != 4 or any(c not in CODE_ALPHABET for c in code):
                raise ApiError(400, "bad_request", "Claim code must be 4 letters/digits")
            hexpart = device_id[4:]
            if not device_id.startswith("pbl-") or len(hexpart) != 12 or any(
                c not in "0123456789abcdef" for c in hexpart
            ):
                raise ApiError(400, "bad_request", "device_id must be pbl- + 12 hex")
            with stub.lock:
                if code in stub.used_codes:
                    raise ApiError(410, "code_expired", "Code expired, making a new one")
                entry = stub.pending.get(code)
                if entry is None:
                    entry = {"device_id": device_id, "created": now(), "approved": False,
                             "fw": str(data.get("fw", "")), "model": str(data.get("model", ""))}
                    stub.pending[code] = entry
                    stub.note(f"claim code {code} from {device_id} waiting")
                if entry["device_id"] != device_id:
                    raise ApiError(409, "code_in_use", "Code in use, making a new one")
                age = now() - entry["created"]
                if age > CODE_TTL_S:
                    stub.pending.pop(code, None)
                    stub.used_codes.add(code)
                    raise ApiError(410, "code_expired", "Code expired, making a new one")
                auto = stub.args.auto_claim
                if auto is not None and age >= auto:
                    entry["approved"] = True
                if not entry["approved"]:
                    self._json(202, {"status": "waiting", "poll_after_s": 3,
                                     "expires_in_s": int(CODE_TTL_S - age)})
                    return 202
                token = "pbt_" + secrets.token_urlsafe(32)
                old = stub.devices.get(device_id, {})
                device = {
                    "token_hash": token_hash(token),
                    "owner": stub.args.owner,
                    "nickname": old.get("nickname", "Pebble"),
                    "away_allowed": False,
                    "talk": old.get("talk", {"hold": True, "toggle": False, "wake": False,
                                             "wake_phrase": "hey stonepi"}),
                    "rev": int(old.get("rev", 0)) + 1,
                    "fw": entry["fw"],
                    "model": entry["model"],
                    "created": now(),
                    "last_seen": now(),
                    "last_path": kind,
                }
                stub.devices[device_id] = device
                stub.pending.pop(code, None)
                stub.used_codes.add(code)
                stub.save()
                stub.note(f"claimed {code} -> {device_id}")
                self._json(200, {"token": token, "device_id": device_id, "owner": device["owner"],
                                 "config": stub.config(device)})
                return 200

        def _mood(self, device_id: str, device: dict) -> int:
            with stub.lock:
                last = stub.last_mood_poll.get(device_id, 0)
                if now() - last < MOOD_MIN_INTERVAL_S:
                    wait = math.ceil(MOOD_MIN_INTERVAL_S - (now() - last))
                    raise ApiError(429, "rate_limited", "Slow down a little", wait)
                stub.last_mood_poll[device_id] = now()
                if stub.pinned_mood:
                    mood = stub.pinned_mood
                else:
                    mood = MOODS[stub.mood_index % len(MOODS)]
                    stub.mood_index += 1
                    stub.mood_since = now()
                payload = {"mood": mood, "since": iso(stub.mood_since),
                           "reason": "stub" if not stub.pinned_mood else "pinned on the stub page",
                           "caption": MOOD_CAPTIONS[mood], "config_rev": device["rev"]}
            self._json(200, payload)
            return 200

        def _ask(self, device: dict) -> int:
            ctype = self.headers.get("Content-Type", "").split(";")[0].strip().lower()
            body = self._body(ASK_MAX_BYTES)
            if ctype not in {"audio/wav", "audio/x-wav", "audio/wave"}:
                raise ApiError(415, "unsupported_audio", "Send 16 kHz mono 16-bit WAV")
            if not stub.ask_lock.acquire(blocking=False):
                raise ApiError(409, "busy", "Pebble is already answering", 5)
            try:
                with stub.lock:
                    forced, stub.next_error = stub.next_error, ""
                if forced:
                    status, message, retry = FORCEABLE_ERRORS[forced]
                    time.sleep(min(stub.args.think_s, 1.0))
                    raise ApiError(status, forced, message, retry)
                samples = read_ask_wav(body)
                seconds = len(samples) / ASK_RATE
                if seconds > ASK_MAX_S + 0.5:
                    raise ApiError(413, "too_large", "That was too long")
                level = level_dbfs(samples)
                if seconds < 0.3 or level < -60:
                    raise ApiError(422, "no_speech", "Didn't catch that")
                time.sleep(stub.args.think_s)
                wav = reply_wav(samples)
                transcript = f"(stub) {seconds:.1f} s of audio at {level:.0f} dBFS"
                reply = f"Stub here, {device['nickname']}. I heard {seconds:.1f} seconds; here it is back."
                self._send(200, wav, "audio/wav", {
                    "X-Pebble-Transcript": header_text(transcript),
                    "X-Pebble-Reply": header_text(reply),
                    "X-Pebble-Mood": "proud",
                })
                stub.note(f"ask: {transcript}")
                return 200
            finally:
                stub.ask_lock.release()

        # control page (stands in for Notify -> Sidekick)
        def _control(self, method: str, path: str) -> None:
            if method == "POST":
                length = int(self.headers.get("Content-Length") or 0)
                form = {k: v[-1] for k, v in parse_qs(self.rfile.read(length).decode()).items()}
                self._control_post(path, form)
                self.send_response(303)
                self.send_header("Location", "/")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if path != "/":
                return self._json(404, {"error": "not_found", "message": "Not found"})
            self._send(200, render_page(stub).encode(), "text/html; charset=utf-8")

        def _control_post(self, path: str, form: dict) -> None:
            with stub.lock:
                if path == "/stub/claim":
                    code = form.get("code", "").strip().upper()
                    if code in stub.pending:
                        stub.pending[code]["approved"] = True
                        stub.note(f"control: approved {code}")
                    else:
                        stub.note(f"control: no waiting device shows {code!r}")
                elif path == "/stub/device":
                    device = stub.devices.get(form.get("device_id", ""))
                    if device:
                        device["nickname"] = form.get("nickname", device["nickname"])[:24] or "Pebble"
                        device["away_allowed"] = form.get("away") == "on"
                        device["talk"] = {"hold": form.get("hold") == "on",
                                          "toggle": form.get("toggle") == "on",
                                          "wake": form.get("wake") == "on",
                                          "wake_phrase": form.get("wake_phrase", "hey stonepi")[:32]}
                        stub.bump(device)
                        stub.save()
                        stub.note(f"control: updated {form.get('device_id')} (rev {device['rev']})")
                elif path == "/stub/revoke":
                    if stub.devices.pop(form.get("device_id", ""), None):
                        stub.save()
                        stub.note(f"control: revoked {form.get('device_id')}")
                elif path == "/stub/mood":
                    mood = form.get("mood", "")
                    stub.pinned_mood = mood if mood in MOODS else ""
                    stub.mood_since = now()
                elif path == "/stub/next-error":
                    err = form.get("error", "")
                    stub.next_error = err if err in FORCEABLE_ERRORS else ""
                elif path == "/stub/lan":
                    stub.lan_down = form.get("down") == "1"
                    stub.note(f"control: LAN {'down' if stub.lan_down else 'up'}")

    return Handler


def render_page(stub: Stub) -> str:
    e = html.escape
    with stub.lock:
        hosts = stub.hosts({"away_allowed": True})
        pending = [
            f"<tr><td><b>{e(code)}</b></td><td>{e(p['device_id'])}</td>"
            f"<td>{int(now() - p['created'])} s</td><td>{'approved' if p['approved'] else 'waiting'}</td>"
            f"<td><form method=post action=/stub/claim><input type=hidden name=code value='{e(code)}'>"
            f"<button>Claim</button></form></td></tr>"
            for code, p in stub.pending.items()
        ]
        devices = []
        for device_id, d in stub.devices.items():
            t = d["talk"]
            chk = lambda on: "checked" if on else ""  # noqa: E731
            seen = f"{int(now() - d.get('last_seen', now()))} s ago via {e(d.get('last_path', '?'))}"
            devices.append(
                f"<div class=card><b>{e(device_id)}</b> · fw {e(d.get('fw', '') or '?')} · "
                f"battery {e(d.get('battery', '') or '?')} · seen {seen} · rev {d['rev']}"
                f"<form method=post action=/stub/device><input type=hidden name=device_id value='{e(device_id)}'>"
                f"<label>Nickname <input name=nickname value='{e(d['nickname'])}'></label> "
                f"<label><input type=checkbox name=away {chk(d.get('away_allowed'))}> Away from home</label> "
                f"<label><input type=checkbox name=hold {chk(t.get('hold'))}> Hold</label> "
                f"<label><input type=checkbox name=toggle {chk(t.get('toggle'))}> Toggle</label> "
                f"<label><input type=checkbox name=wake {chk(t.get('wake'))}> Wake</label> "
                f"<label>Phrase <input name=wake_phrase value='{e(t.get('wake_phrase', ''))}'></label> "
                f"<button>Save (rev+1)</button></form>"
                f"<form method=post action=/stub/revoke><input type=hidden name=device_id value='{e(device_id)}'>"
                f"<button class=danger>Revoke</button></form></div>"
            )
        mood_opts = "".join(
            f"<option value='{m}' {'selected' if stub.pinned_mood == m else ''}>{m}</option>" for m in MOODS
        )
        err_opts = "".join(
            f"<option value='{k}' {'selected' if stub.next_error == k else ''}>{k}</option>"
            for k in FORCEABLE_ERRORS
        )
        log = "\n".join(e(line) for line in stub.log)
        lan_down = stub.lan_down
    return f"""<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width, initial-scale=1"><title>Pebble stub</title>
<style>
body{{font:14px system-ui,sans-serif;margin:16px;max-width:900px;background:#faf8f4;color:#16191b}}
h1{{font-size:20px}} h2{{font-size:15px;margin-top:24px}} code{{background:#eee;padding:1px 4px}}
table{{border-collapse:collapse}} td{{padding:4px 10px 4px 0}} .card{{background:#fff;border:1px solid #ddd;
border-radius:10px;padding:10px;margin:8px 0}} .card form{{margin-top:6px}} input{{margin-right:6px}}
button{{cursor:pointer}} .danger{{color:#a00}} pre{{background:#fff;border:1px solid #ddd;padding:8px;
max-height:320px;overflow:auto;font-size:12px}} .warn{{color:#a00;font-weight:600}}
</style></head><body>
<h1>Pebble stub · protocol v1</h1>
<p>Device base URL: <code>{e(hosts['lan'])}{BASE}</code> (LAN) ·
<code>{e(hosts['away'])}{BASE}</code> (away stand-in; real one is HTTPS :8443).
Owner for new claims: <b>{e(stub.args.owner)}</b>. <a href=/>Refresh</a></p>

<h2>Waiting claim codes</h2>
<table>{''.join(pending) or '<tr><td>None. Open Settings → Pairing on the device.</td></tr>'}</table>
<form method=post action=/stub/claim>Enter a code like Sidekick would:
<input name=code size=5 maxlength=4> <button>Claim</button></form>

<h2>Devices</h2>{''.join(devices) or '<p>No paired devices.</p>'}

<h2>Test controls</h2>
<form method=post action=/stub/mood>Mood: <select name=mood><option value=''>cycle all 7 (each poll)</option>
{mood_opts}</select> <button>Set</button></form>
<form method=post action=/stub/next-error>Next ask fails with: <select name=error><option value=''>none</option>
{err_opts}</select> <button>Set</button></form>
<form method=post action=/stub/lan>LAN port is <span class={'warn' if lan_down else ''}>{'DROPPING connections' if lan_down else 'up'}</span>
<input type=hidden name=down value='{0 if lan_down else 1}'>
<button>{'Bring LAN back' if lan_down else 'Drop LAN (test failover)'}</button></form>

<h2>Log</h2><pre>{log}</pre>
</body></html>"""


def main() -> None:
    here = Path(__file__).resolve().parent
    p = argparse.ArgumentParser(description="Pebble protocol v1 stub (stands in for StonePi AgentLink).")
    p.add_argument("--host", default="0.0.0.0", help="bind address (default all interfaces)")
    p.add_argument("--lan-port", type=int, default=8014)
    p.add_argument("--away-port", type=int, default=8016)
    p.add_argument("--public-host", default="", help="address put in hosts.lan/away (default: this PC's LAN IP)")
    p.add_argument("--owner", default="Stub user", help="owner name returned on claim")
    p.add_argument("--auto-claim", type=float, default=None, metavar="SECONDS",
                   help="approve any claim code this long after its first poll")
    p.add_argument("--think-s", type=float, default=1.5, help="fake thinking time per ask")
    p.add_argument("--state", default=str(here / ".pebble_stub_state.json"), help="paired devices file")
    args = p.parse_args()
    args.public_host = args.public_host or lan_ip()

    stub = Stub(args)
    servers = []
    for kind, port in (("lan", args.lan_port), ("away", args.away_port)):
        srv = ThreadingHTTPServer((args.host, port), make_handler(stub, kind))
        srv.daemon_threads = True
        servers.append(srv)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("Pebble stub (protocol v1)")
    print(f"  LAN   http://{args.public_host}:{args.lan_port}{BASE}   control page http://{args.public_host}:{args.lan_port}/")
    print(f"  Away  http://{args.public_host}:{args.away_port}{BASE}   (stand-in for Funnel HTTPS :8443)")
    if args.auto_claim is not None:
        print(f"  Auto-claim after {args.auto_claim:g} s")
    print("Ctrl+C to stop.", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        for srv in servers:
            srv.shutdown()


if __name__ == "__main__":
    main()
