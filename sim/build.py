#!/usr/bin/env python3
"""Build the PC simulator (no ESP-IDF). Same idea as The-Badge's sim/build.py.

    python sim/build.py
    # needs: mingw-w64 gcc on Windows, or gcc on Linux/macOS
    # LVGL: run python sim/setup_sim.py once first
"""
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.chdir(HERE)


def _find_lvgl():
    for c in (Path("../managed_components/lvgl__lvgl"), Path("lvgl")):
        if (c / "lv_version.h").exists():
            return c
    return Path("lvgl")


LVGL = _find_lvgl()
CC = os.environ.get("CC", "gcc")
AR = os.environ.get("AR", "ar")
JOBS = int(os.environ.get("SIM_JOBS") or os.cpu_count() or 4)

CFLAGS = ["-O1", "-w", "-DPEBBLE_SIM", "-DLV_CONF_INCLUDE_SIMPLE"]
OUR_CFLAGS = [f for f in CFLAGS if f != "-w"] + [
    "-Werror=switch",
    "-Werror=implicit-function-declaration",
    "-Werror=return-type",
    "-Werror=incompatible-pointer-types",
    "-Werror=int-conversion",
]
if os.name == "nt":
    CFLAGS += ["-include", "win_compat.h"]
    OUR_CFLAGS += ["-include", "win_compat.h"]

INC = ["-I.", "-I../main", f"-I{LVGL}", f"-I{LVGL}/src"]

ESP_ONLY = {
    "main.c",
    "port_esp.c",
    "display.c",
}

PEBBLE_SRCS = [
    Path("main_sim.c"),
    Path("port_sim.c"),
    *[p for p in sorted(Path("../main").glob("*.c")) if p.name not in ESP_ONLY],
    *sorted(Path("../main/assets").glob("*.c")),
]


def die(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def obj_for(src: Path, into: str) -> Path:
    flat = src.as_posix().replace("/", "_").replace(".", "_")
    return Path(into) / (flat + ".o")


def is_fresh(src: Path, obj: Path) -> bool:
    if not obj.exists():
        return False
    t = obj.stat().st_mtime
    if t < src.stat().st_mtime:
        return False
    dep = obj.with_name(obj.name[:-2] + ".d")
    if not dep.exists():
        return False
    try:
        text = dep.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    text = text.replace("\\\n", " ")
    _, _, rhs = text.partition(":")
    for w in rhs.split():
        try:
            if os.stat(w).st_mtime > t:
                return False
        except OSError:
            return False
    return True


def flags_for(src: Path):
    p = src.as_posix()
    ours = p.startswith("main_sim") or p.startswith("port_sim") or "/main/" in p
    return OUR_CFLAGS if ours else CFLAGS


def compile_one(src: Path, into: str, skip_fresh: bool):
    obj = obj_for(src, into)
    if skip_fresh and is_fresh(src, obj):
        return None
    tmp = obj.parent / (obj.name + ".part")
    dep = obj.with_name(obj.name[:-2] + ".d")
    r = subprocess.run(
        [CC, *flags_for(src), *INC, "-MMD", "-MF", str(dep), "-c", str(src), "-o", str(tmp)],
        capture_output=True,
        text=True,
        errors="replace",
    )
    if r.returncode != 0:
        tmp.unlink(missing_ok=True)
        return f"{src}\n{r.stdout}{r.stderr}"
    os.replace(tmp, obj)
    return None


def compile_all(srcs, into: str, skip_fresh: bool, label: str):
    Path(into).mkdir(exist_ok=True)
    todo = [s for s in srcs if not (skip_fresh and is_fresh(s, obj_for(s, into)))]
    if not todo:
        return
    print(f"{label}: building {len(todo)} (workers {JOBS})", flush=True)
    done = 0
    with ThreadPoolExecutor(max_workers=JOBS) as pool:
        for err in pool.map(lambda s: compile_one(s, into, skip_fresh), todo):
            done += 1
            if err:
                die(f"\ncompile failed\n{err}")
            if done % 50 == 0 or done == len(todo):
                print(f"  {done}/{len(todo)}", flush=True)


def main():
    if not LVGL.is_dir() or not (LVGL / "lv_version.h").exists():
        die(
            f"LVGL missing at {LVGL}\n"
            "  run:  python sim/setup_sim.py\n"
            "  or build firmware once so managed_components/lvgl__lvgl exists"
        )
    if shutil.which(CC) is None:
        die(f"{CC} not found. On Windows install mingw-w64 and put gcc on PATH.")

    lvgl_srcs = sorted(LVGL.glob("src/**/*.c"))
    ver = "?"
    vh = LVGL / "lv_version.h"
    if vh.exists():
        nums = {}
        for line in vh.read_text(encoding="utf-8", errors="replace").splitlines():
            for key in ("MAJOR", "MINOR", "PATCH"):
                tag = f"#define LVGL_VERSION_{key}"
                if line.startswith(tag):
                    nums[key] = line[len(tag) :].strip()
        if len(nums) == 3:
            ver = f"{nums['MAJOR']}.{nums['MINOR']}.{nums['PATCH']}"
    stamp = f"lvgl {ver} · {len(lvgl_srcs)} files | " + " ".join(CFLAGS)
    stamp_f = Path("liblvgl.stamp")
    lib = Path("liblvgl.a")
    stale = (
        os.environ.get("SIM_LVGL_REBUILD")
        or not lib.exists()
        or not stamp_f.exists()
        or stamp_f.read_text(encoding="utf-8") != stamp
    )
    if stale:
        lib.unlink(missing_ok=True)
        stamp_f.unlink(missing_ok=True)
        compile_all(lvgl_srcs, "lvgl_obj", True, "LVGL")
        objs = [str(obj_for(s, "lvgl_obj")) for s in lvgl_srcs]
        part = Path(str(lib) + ".part")
        part.unlink(missing_ok=True)
        for i in range(0, len(objs), 80):
            r = subprocess.run(
                [AR, "rcs", str(part), *objs[i : i + 80]],
                capture_output=True,
                text=True,
                errors="replace",
            )
            if r.returncode != 0:
                part.unlink(missing_ok=True)
                die(f"ar failed\n{r.stdout}{r.stderr}")
        os.replace(part, lib)
        stamp_f.write_text(stamp, encoding="utf-8")
        print(f"  -> sim/liblvgl.a ({len(objs)} objects)")

    compile_all(PEBBLE_SRCS, "pebble_obj", True, "pebble")

    out = "pebble_sim"
    if sys.platform == "darwin":
        libargs = [str(lib)]
    else:
        libargs = ["-Wl,--start-group", str(lib), "-Wl,--end-group"]
    cmd = [CC, *[str(obj_for(s, "pebble_obj")) for s in PEBBLE_SRCS], *libargs, "-lm", "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        die(f"link failed\n{r.stdout}{r.stderr}")
    made = Path(out + ".exe") if Path(out + ".exe").exists() else Path(out)
    print(f"build finished -> sim/{made.name} ({made.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
