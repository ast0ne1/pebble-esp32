#!/usr/bin/env python3
"""Fetch LVGL for the simulator only (no ESP-IDF).

Clones into sim/lvgl at the version in main/idf_component.yml so firmware
Component Manager and the sim do not fight over managed_components/.
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
YML = ROOT / "main" / "idf_component.yml"
DEST = HERE / "lvgl"


def version() -> str:
    text = YML.read_text(encoding="utf-8")
    # lvgl/lvgl: version: "9.5.0"
    m = re.search(r"lvgl/lvgl:\s*\n\s*version:\s*[\"']?([0-9]+\.[0-9]+\.[0-9]+)", text)
    if not m:
        sys.exit(f"could not parse LVGL version from {YML}")
    return m.group(1)


def main():
    if (DEST / "lv_version.h").exists():
        print(f"already have {DEST}")
        return
    ver = version()
    print(f"cloning LVGL v{ver} -> {DEST}")
    subprocess.check_call(
        ["git", "clone", "--depth", "1", "-b", f"v{ver}", "https://github.com/lvgl/lvgl.git", str(DEST)]
    )
    print("ok — next: python sim/build.py")


if __name__ == "__main__":
    main()
