# Pebble firmware setup

## Hardware

| | |
|---|---|
| Board | Waveshare **ESP32-S3-Touch-AMOLED-1.75C** (aluminium / 32 MB) |
| Not | Plain **1.75** (16 MB) — wrong flash size / partitions |

### Buttons (1.75C)

| Button | Wiring | Firmware |
|---|---|---|
| BOOT | GPIO0, active low | Hold ≥220 ms → PTT (`shell_ptt_set`); short → wake |
| PWR | TCA9554/A **EXIO4** (I2C probe `0x20` then `0x38`), high while pressed (not GPIO3) | Short → home/wake; hold ~1.5 s → soft sleep |

### Wi‑Fi

Settings → Wi‑Fi → **Scan** → tap SSID → password → **Join**. Credentials land in NVS (`wificfg`); next boot auto-rejoins. Pairing uses PROTOCOL v1 claim poll against AgentLink (or the local stub).

## Toolchain

**ESP-IDF 6.0.x** — developed and flashed against **v6.0.1**. Do not assume IDF 6.1
works until someone verifies it. Component Manager allows `idf >= 5.5.0`, but
6.0.x is what we build with (cJSON via Component Manager, esp-sr, LVGL 9.5).

Install IDF 6.0.1 via the Espressif installer or clone, then open a shell where
`IDF_PATH` is set (or source `export.ps1` / `export.sh` from that install).

Windows (PowerShell):

```powershell
. $env:IDF_PATH\export.ps1   # or: . <idf-install>\esp-idf\export.ps1
cd <this-repo>
idf.py set-target esp32s3
# Windows: esp-sr model packer needs UTF-8 console
$env:PYTHONUTF8 = "1"
idf.py build
# plug USB — note the COM port in Device Manager, then:
idf.py -p COMx flash monitor
```

Linux / macOS: source `$IDF_PATH/export.sh`, then the same `idf.py` commands.

First `build` pulls LVGL + Waveshare BSP + esp-sr into `managed_components/` (gitignored).
Full flash must include the **model** partition (`0x510000` / `srmodels.bin`) — `idf.py flash` and `tools/mkflash.py` both do.

### Device test checklist

1. Board is **1.75C** (32 MB aluminium), USB data cable (not charge-only).
2. `idf.py build` succeeds (see above).
3. Plug device → confirm a new COM port appears.
4. Flash + serial: `idf.py -p COMx flash monitor`.
5. On boot expect: splash → home face/clock. Swipe to Settings → Wi‑Fi scan/join.
6. Buttons: **BOOT** hold = PTT; **PWR** short = home; **PWR** hold ≈ soft sleep. Log should show `TCA9554 at 0x..`.
7. Voice: tap face → chirp; hold BOOT/face → speak → release. Unpaired = loopback; paired = `POST /ask`.
8. Pairing: Settings → Pairing → set Host to `http://<Pi-IP>` (appliance) or `http://<PC-LAN-IP>:8015` (AgentLink on a PC). Enter the claim code in StonePi → Notify → Sidekick.
9. Wake (optional): enable `talk.wake` in Sidekick (or the stub control page). Say **Hi ESP** → timed listen window → speak → auto-release asks.

## Flash

### WebSerial (browser)

After a successful `idf.py build`:

```powershell
python tools\mkflash.py
python -m http.server 8080 --directory flash
# Chrome/Edge → http://localhost:8080/
```

Hold **BOOT**, plug USB, release, then **Connect & flash**. First install: erase when prompted.
`flash/pebble-merged.bin` includes bootloader, partitions, app, and the ESP-SR **model** partition.

### esptool / idf.py

```powershell
idf.py -p COMx flash monitor
# or, from build/:
# python -m esptool --chip esp32s3 -p COMx write_flash @flash_args
```

Binaries land in `build/` after `idf.py build`. CI-published images are a later step.

## Talking to AgentLink / the stub

Pi-side AgentLink is part of [StonePi](https://github.com/ast0ne1/stonePi).

**StonePi appliance:** Host `http://<Pi-IP>` — no port (nginx on :80 → AgentLink).
Do **not** use `:8015` on the Pi.

**AgentLink on a PC (dev):** Host `http://<this-PC-LAN-IP>:8015`.

**Protocol stub** (no AgentLink):

```powershell
python tools\pebble_stub.py --auto-claim 5
# control page: http://127.0.0.1:8014/
# device Host field: http://<this-PC-LAN-IP>:8014
```

## Do not commit

| | |
|---|---|
| `sdkconfig` | regenerated from `sdkconfig.defaults` |
| `build/` | artifacts |
| `managed_components/` | Component Manager |
| `main/secrets.h` | Wi‑Fi passwords if you add one |
| `HANDOFF.md` | internal only (gitignored) |

## PC simulator (preferred for UI)

Runs the **same** `main/*.c` UI as the board (minus `main.c` / `port_esp.c` / `display.c`).

```powershell
python sim\setup_sim.py
python sim\build.py          # needs mingw-w64 gcc on PATH
.\sim\pebble_sim.exe         # PNGs in sim\shots\
$env:PORT=8792; python sim\server.py
# browser → http://localhost:8792
```
