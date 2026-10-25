# Pebble mic / PTT audio

Hardware: Waveshare **ESP32-S3-Touch-AMOLED-1.75C** — mics via **ES7210**, speaker via
**ES8311** + NS4150B (PA GPIO46). Reuse Muse’s **hold → capture → release** UX only;
do not copy Muse’s AIC3204 board code.

## Current: loopback (v1)

Hold face or **BOOT** → record into PSRAM → release → play the same clip on the
speaker. Moods: listening → thinking → speaking → idle.

| API | Role |
| --- | --- |
| `bsp_audio_init(NULL)` | Duplex mono **22050 Hz** 16-bit I2S (BSP default) |
| `bsp_audio_codec_microphone_init()` | ES7210 |
| `bsp_audio_codec_speaker_init()` | ES8311 |
| `voice_ptt_down` / `voice_ptt_up` | Wired from `shell_ptt_set` |

Cap ~10 s mono PCM in PSRAM for loopback (unpaired). No resample on loopback.
When paired, release starts the PROTOCOL ask path (see below).
Loopback is peak-normalized to the same ~24000 target as the chirp assets so
speech plays at a similar level (mic capture is quieter than baked SFX).

### Nudge chirps

Short baked PCM clips (`pcm_nudge0..2` at 22050 Hz) play on face tap via
`voice_play_nudge`. Speaker stays open between chirps (close/reopen was one-shot
on this BSP). Skipped while PTT/loopback is busy. Chirps respect Settings →
Sound volume + Chirps on/off.

Muse open gadget SDK has no local tap/avatar SFX assets (reply audio is MP3/TTS
only); these chirps are Pebble-local.

There is **no** `bsp_audio_init_voice_24k` in BSP 3.0.0 — that name was from older notes.

## Pins (BSP-owned)

| Signal | GPIO |
| --- | ---: |
| I2S MCLK | 16 |
| I2S BCLK | 9 |
| I2S LRCK | 45 |
| Playback DIN (ES8311) | 8 |
| Capture DOUT (ES7210) | 10 |
| PA_CTRL | 46 |

## Sidekick ask

[`PROTOCOL.md`](../PROTOCOL.md) v1: `POST /agent/pebble/v1/ask` wants **16 kHz mono**
WAV, so resample 22 050 → 16 000 on post only. The reply WAV is **22 050 Hz** mono,
the BSP's native rate, so it plays on the ES8311 as-is. Test against
`tools/pebble_stub.py`, which answers with a chime plus your own clip.

## Wake word (ESP-SR)

When `talk.wake` is on (`wake.c`):

- Mic opens at **16 kHz** for AFE + WakeNet (**Hi ESP**) + English MultiNet (`wake_phrase`).
- On detect, shell opens a timed PTT window (`max_ask_s`, capped 3–15 s), then ask/loopback as usual.
- `wake_pause` / `wake_resume` hand the codec to PTT/playback so rates do not fight.
- Models live in the `model` flash partition (packed by esp-sr at build time).

## Buttons

| Input | Short | Hold |
| --- | --- | --- |
| Face (home) | Tap mood | PTT |
| BOOT GPIO0 | Wake | PTT |
| PWR TCA9554 EXIO4 | Wake + home | Soft sleep (~1.5 s) |

Firmware probes I2C `0x20` (TCA9554) then `0x38` (TCA9554A) for the expander.
