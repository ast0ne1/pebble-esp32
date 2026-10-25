# Pebble ↔ StonePi protocol v1

The contract between this firmware and [StonePi](https://github.com/ast0ne1/stonePi).
On the appliance, **AgentLink** (in that repo) serves these paths — LAN usually
via nginx on port 80; the service itself listens on **8015**. For offline / bench
testing, [`tools/pebble_stub.py`](tools/pebble_stub.py) implements the same
contract (LAN :8014, away stand-in :8016).

Anything not written here is not part of the contract.

## 1. Basics

| | |
|---|---|
| Base path | `/agent/pebble/v1` |
| Format | JSON (`application/json`, UTF-8) except the ask body and reply (`audio/wav`) |
| Auth | `Authorization: Bearer <token>` on every call except claim |
| Versioning | The `v1` in the path. The Pi may **add** JSON fields and headers; the device must ignore unknown ones. Anything breaking becomes `v2`. |

Every request should also send:

| Header | Example | |
|---|---|---|
| `X-Pebble-Device` | `pbl-a0b1c2d3e4f5` | Device id (`pbl-` + Wi-Fi MAC, lowercase hex, no colons). Same value as in the claim. |
| `X-Pebble-Fw` | `0.1.0` | Firmware version. Shown in Sidekick. |
| `X-Pebble-Battery` | `87` | Optional. Percent, or omit when unknown / no pack. |

## 2. Hosts and transport

A paired device knows two base URLs (from the claim reply and `GET /config`):

| Host | Example | Transport | When |
|---|---|---|---|
| `lan` | `http://192.168.1.20` | Plain HTTP | At home. Fast, works when the internet is down. |
| `away` | `https://stonepi.tail1234.ts.net:8443` | HTTPS (public certificate, Tailscale Funnel) | Anywhere with Wi-Fi. `null` when the owner hasn't allowed away use. |

Full URL = host + base path, e.g. `http://192.168.1.20/agent/pebble/v1/mood`.

**Choosing a host:**

1. Try the host that worked last (start with `lan`).
2. Connect timeout **1.5 s** on `lan`, **5 s** on `away`. Read timeout 10 s, or **40 s** for `POST /ask`.
3. Switch to the other host and retry **once** only on a transport failure: DNS, connect, TLS, timeout, connection reset, or HTTP `502` / `504`. Never switch on any other status. Those are real answers.
4. Remember which host worked.

**HTTPS on `away`:** verify the certificate against the normal public CA bundle
(`esp_crt_bundle_attach`). It's a Let's Encrypt certificate that renews on its
own, so don't pin it. Never turn verification off.

**`away` is limited by design.** It only carries `/agent/pebble/v1/*` minus
claim, and only for devices whose owner turned on **Away from home** in
Sidekick (off by default). Otherwise it answers `403 away_disabled`.

## 3. Errors

Every non-2xx reply has a JSON body:

```json
{ "error": "busy", "message": "Pebble is already answering", "retry_after_s": 5 }
```

`message` is short (≤ 60 characters) and fine to show on screen. `retry_after_s`
is present only when waiting helps; it matches the `Retry-After` header.

| Status | `error` | Device should |
|---|---|---|
| 400 | `bad_request` | Bug in the request. Log it. |
| 401 | `unauthorized` | **The token is revoked or unknown.** Clear the pairing (`port_pair_clear`) and go back to the claim screen. Only `401` does this. |
| 403 | `away_disabled` | Away use is off for this device. Keep the pairing. Show the message; retry `lan` later. |
| 403 | `owner_disabled` | The owner's StonePi account was disabled. Keep the pairing (it may be re-enabled). Show the message. |
| 404 | `not_found` | Wrong path, or claim attempted over `away`. |
| 409 | `busy` | Another ask is running. Show the message; the user can try again. |
| 409 | `code_in_use` | Claim only: pick a new claim code. |
| 410 | `code_expired` | Claim only: pick a new claim code. |
| 413 | `too_large` | Recording too long or too big. |
| 415 | `unsupported_audio` | Not 16 kHz mono 16-bit PCM WAV. |
| 422 | `no_speech` | Nothing intelligible heard. Show "Didn't catch that". |
| 429 | `rate_limited` | Wait `retry_after_s`. |
| 503 | `ai_unavailable` | AI isn't set up or every model failed. Upset face + message. |
| 503 | `speech_unavailable` | Speech-to-text or text-to-speech is down. Upset face + message. |
| 500 | `internal` | Show the message. |

## 4. Claim (pairing)

Claiming only works over `lan` (the Pi you're on), never over `away`.

**Claim code:** 4 characters from `ACDEFHJKMNPRTUVWXY34679` (no lookalikes:
no 0/O, 1/I/L, 2/Z, 5/S, 8/B). The device makes it up, shows it in
Settings → Pairing, and polls. The user enters it in StonePi under
**Notify → Sidekick → Add Pebble** while signed in. The device then belongs to
that user.

**Where to poll:** an unpaired device has no host yet. It polls the LAN address
the user typed into Settings → Pairing (default **empty** — user must enter it).
On the appliance use `http://<Pi-IP>` (nginx :80, no port). Direct to AgentLink
on a PC: `http://<PC-IP>:8015`. Stub: `http://<PC-IP>:8014`. A `.local` name is
only valid once the device runs `espressif/mdns` (`mdns_init` after Wi‑Fi);
firmware does not default to `.local` today. The Pi's hostname is configurable,
so the field must be editable.

```
POST /agent/pebble/v1/claim/poll
Content-Type: application/json

{ "code": "K7M4", "device_id": "pbl-a0b1c2d3e4f5", "fw": "0.1.0", "model": "waveshare-amoled-175c" }
```

| Reply | Meaning |
|---|---|
| `202 { "status": "waiting", "poll_after_s": 3, "expires_in_s": 840 }` | Keep polling every `poll_after_s`. |
| `200` claim reply (below) | Claimed. Save to NVS and stop polling. |
| `409 code_in_use` | Another device holds that code. Pick a new one. |
| `410 code_expired` | Code older than **15 minutes**, or already used. Pick a new one. |
| `429 rate_limited` | Polling faster than once a second. |

A code is tied to the `device_id` of its first poll. The token is handed out
**once**; the next poll with that code gets `410`.

Claim reply:

```json
{
  "token": "pbt_3q2-7wE…(43 chars)",
  "device_id": "pbl-a0b1c2d3e4f5",
  "owner": "Alex",
  "config": { …same as GET /config… }
}
```

Tokens are `pbt_` + 43 base64url characters (47 total). Store it as-is.

## 5. Config

```
GET /agent/pebble/v1/config
200
{
  "rev": 3,
  "nickname": "Kitchen",
  "character": "Pebble",
  "owner": "Alex",
  "hosts": { "lan": "http://192.168.1.20", "away": "https://stonepi.tail1234.ts.net:8443" },
  "away_allowed": true,
  "talk": { "hold": true, "toggle": false, "wake": false, "wake_phrase": "hey stonepi" },
  "mood_poll_s": 30,
  "max_ask_s": 15
}
```

- `hosts.away` is `null` when away use is off.
- `talk.*` are the owner's choices in Sidekick. Wake word is optional for boards
  that can't run it; ignore `wake` there.
- `rev` goes up whenever anything here changes. Every mood reply carries
  `config_rev`; when it differs from the saved `rev`, fetch `GET /config`.

## 6. Mood

```
GET /agent/pebble/v1/mood
200
{
  "mood": "worried",
  "since": "2026-10-22T20:00:00Z",
  "reason": "NewsCast isn't running",
  "caption": "Something needs a look",
  "config_rev": 3
}
```

- `mood` is one of the 7 base moods: `happy`, `content`, `sleepy`, `curious`,
  `worried`, `upset`, `proud`. Treat an unknown value as `content`.
- `listening` / `thinking` / `speaking` are device-local states during an ask.
  The Pi never sends them.
- `caption` is optional, short (≤ 40 characters) and fine to show.
- Works with AI switched off.
- Poll every `mood_poll_s` (default 30) while idle. Back off more on battery if
  you like. Faster than every 10 s gets `429`.

## 7. Ask

```
POST /agent/pebble/v1/ask
Content-Type: audio/wav
Authorization: Bearer <token>

<WAV: RIFF/PCM, 16 000 Hz, mono, 16-bit little-endian, ≤ max_ask_s (15 s), ≤ 512 KiB>
```

The 1.75C records at 22 050 Hz, so it resamples to 16 kHz before posting.

**Success:** `200 audio/wav`: PCM WAV, **22 050 Hz** mono 16-bit (Piper's rate,
and what the 1.75C plays natively). Extra headers:

| Header | |
|---|---|
| `X-Pebble-Transcript` | What the Pi heard. |
| `X-Pebble-Reply` | The reply text, for a caption. |
| `X-Pebble-Mood` | Optional mood to show after speaking (e.g. `proud`). |

Text headers are UTF-8, percent-encoded (`%C3%A6` for æ), at most 200 bytes
before encoding. Spaces and common punctuation are usually left as-is, but any
byte may be encoded, so decode every `%XX`.

**Errors:** `409 busy` (one ask at a time per device, and one across the Pi),
`413`, `415`, `422 no_speech`, `503 ai_unavailable` / `speech_unavailable`.

Keep the mic off while the reply plays.

## 8. Unpair from the device

```
POST /agent/pebble/v1/unpair
204
```

Revokes this token on the Pi. Then clear the pairing locally. If the call
fails, clear it locally anyway; the owner can remove the device in Sidekick.

## 9. Pi side (for reference, not for firmware)

- **LAN:** nginx on port 80 forwards `/agent/pebble/v1/` to AgentLink (`127.0.0.1:8015`),
  never redirected to HTTPS, and sets `X-StonePi-Path: lan` (overwriting any
  value the client sent).
- **Away:** Tailscale Funnel on `:8443` forwards to a separate nginx listener on
  loopback that only serves `/agent/pebble/v1/` (claim → 404) and sets
  `X-StonePi-Path: away`. Funnel is only switched on while at least one device
  has away use allowed.
- Tokens are stored hashed. Each device is an AgentLink binding to its owner.
- Every call is audited. Rate limits apply per token and per source IP.
- The `lan` host is the Pi's current LAN IP. Give the Pi a DHCP reservation; if
  it moves, `away` still works and the next config fetch picks up the new IP.
