#!/usr/bin/env python3
"""Build a WebSerial-ready merged binary + manifest after `idf.py build`.

    python tools/mkflash.py
    # writes flash/pebble-merged.bin + flash/manifest.json

Serve locally (Web Serial allows http://localhost):

    python -m http.server 8080 --directory flash
    # open http://localhost:8080/
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
FLASH = ROOT / "flash"
MERGED = FLASH / "pebble-merged.bin"
MANIFEST = FLASH / "manifest.json"


def die(msg: str) -> None:
    print(msg, file=sys.stderr)
    sys.exit(1)


def find_esptool() -> list[str]:
    # Prefer IDF's python -m esptool; fall back to esptool.py on PATH.
    try:
        subprocess.run(
            [sys.executable, "-m", "esptool", "version"],
            check=True,
            capture_output=True,
        )
        return [sys.executable, "-m", "esptool"]
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    if shutil.which("esptool.py"):
        return ["esptool.py"]
    if shutil.which("esptool"):
        return ["esptool"]
    die("esptool not found — run from an ESP-IDF export shell")


def main() -> None:
    args_path = BUILD / "flasher_args.json"
    if not args_path.is_file():
        die(f"missing {args_path} — run `idf.py build` first")

    data = json.loads(args_path.read_text(encoding="utf-8"))
    flash_files: dict[str, str] = data.get("flash_files") or {}
    if not flash_files:
        die("flasher_args.json has no flash_files")

    # Sort by offset; paths are relative to build/
    parts: list[tuple[int, Path]] = []
    for off_s, rel in flash_files.items():
        off = int(off_s, 0)
        p = BUILD / rel
        if not p.is_file():
            die(f"missing flash part {p}")
        parts.append((off, p))
    parts.sort(key=lambda x: x[0])

    FLASH.mkdir(parents=True, exist_ok=True)
    et = find_esptool()
    cmd = et + [
        "--chip",
        "esp32s3",
        "merge-bin",
        "-o",
        str(MERGED),
        "--flash-mode",
        "dio",  # WebSerial: qio → dio
        "--flash-freq",
        "80m",
        "--flash-size",
        "32MB",
    ]
    for off, p in parts:
        cmd += [f"0x{off:x}", str(p)]

    print(" ".join(cmd))
    subprocess.run(cmd, check=True)

    ver = date.today().isoformat()
    project = {}
    proj_path = BUILD / "project_description.json"
    if proj_path.is_file():
        try:
            project = json.loads(proj_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            project = {}
    app_ver = project.get("app_version") or project.get("project_version") or ver

    manifest = {
        "name": "StonePi Pebble",
        "version": str(app_ver),
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [{"path": "pebble-merged.bin", "offset": 0}],
            }
        ],
    }
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    size_mb = MERGED.stat().st_size / (1024 * 1024)
    print(f"wrote {MERGED} ({size_mb:.2f} MiB)")
    print(f"wrote {MANIFEST}")
    print("serve: python -m http.server 8080 --directory flash")


if __name__ == "__main__":
    main()
