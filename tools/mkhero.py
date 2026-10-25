#!/usr/bin/env python3
"""Bake docs/hero.png for the README (cream paper + device splash).

Needs Pillow. Sources live under tools/art/.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FW = Path(__file__).resolve().parents[1]
ART = FW / "tools" / "art"
OUT = FW / "docs" / "hero.png"

PANEL = 1000
PAPER = (244, 241, 234)
WASH = (239, 232, 220)


def radial_bg(size: int, c0=(42, 38, 32), c1=(11, 13, 14)) -> Image.Image:
    img = Image.new("RGB", (size, size), c1)
    px = img.load()
    cx = cy = (size - 1) / 2.0
    rmax = size * 0.55
    for y in range(size):
        for x in range(size):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 / rmax
            t = min(1.0, d * d)
            px[x, y] = (
                int(c0[0] + (c1[0] - c0[0]) * t),
                int(c0[1] + (c1[1] - c0[1]) * t),
                int(c0[2] + (c1[2] - c0[2]) * t),
            )
    return img


def soft_circle(im: Image.Image, solid: float = 0.92, fade: float = 0.995) -> Image.Image:
    im = im.convert("RGBA")
    w, h = im.size
    cx = cy = (w - 1) / 2.0
    r = min(w, h) / 2.0
    mask = Image.new("L", (w, h), 0)
    mp = mask.load()
    for y in range(h):
        for x in range(w):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 / r
            if d <= solid:
                mp[x, y] = 255
            elif d >= fade:
                mp[x, y] = 0
            else:
                t = (d - solid) / (fade - solid)
                mp[x, y] = int(255 * (1.0 - t))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(im, (0, 0), mask)
    return out


def cover_square(path: Path, size: int, focus_y: float = 0.40, zoom: float = 1.05) -> Image.Image:
    im = Image.open(path).convert("RGB")
    tw = th = int(size * zoom)
    scale = max(tw / im.width, th / im.height)
    nw, nh = int(im.width * scale), int(im.height * scale)
    im = im.resize((nw, nh), Image.Resampling.LANCZOS)
    left = (nw - tw) // 2
    top = max(0, min(nh - th, int((nh - th) * focus_y)))
    im = im.crop((left, top, left + tw, top + th))
    if tw != size:
        im = im.resize((size, size), Image.Resampling.LANCZOS)
    return im


def make_mark(size: int) -> Image.Image:
    """StonePi mark (viewBox 8..92) — supersampled for clean edges."""
    S = size * 4
    m = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(m)

    def pt(x: float, y: float) -> tuple[float, float]:
        return ((x - 8) / 84.0 * S, (y - 8) / 84.0 * S)

    def poly(pts: list[tuple[float, float]], fill: tuple[int, int, int, int]) -> None:
        d.polygon([pt(*p) for p in pts], fill=fill)

    poly([(20, 32), (80, 8), (80, 20), (32, 39.2), (32, 46.2), (20, 51)], (168, 152, 124, 255))
    poly([(20, 56), (80, 32), (80, 44), (20, 68)], (110, 116, 122, 255))
    poly([(80, 68), (20, 92), (20, 80), (68, 60.8), (68, 53.8), (80, 49)], (63, 68, 72, 255))
    return m.resize((size, size), Image.Resampling.LANCZOS)


def load_font(size: int) -> ImageFont.ImageFont:
    for name in ("seguisb.ttf", "segoeuib.ttf", "segoeui.ttf", "arial.ttf"):
        path = Path(r"C:\Windows\Fonts") / name
        if path.is_file():
            return ImageFont.truetype(str(path), size)
        # Linux / macOS common locations
        for base in ("/usr/share/fonts/truetype/dejavu", "/System/Library/Fonts"):
            p = Path(base) / name
            if p.is_file():
                return ImageFont.truetype(str(p), size)
    return ImageFont.load_default()


def compose_splash() -> Image.Image:
    plate = radial_bg(PANEL).convert("RGBA")
    mark = make_mark(int(PANEL * 0.16))
    plate.alpha_composite(mark, ((PANEL - mark.width) // 2, int(PANEL * 0.14)))

    font_hi = load_font(int(PANEL * 0.062))
    font_sub = load_font(int(PANEL * 0.030))
    draw = ImageDraw.Draw(plate)
    for text, font, y, fill in (
        ("Hi — I'm Pebble", font_hi, PANEL * 0.33, (244, 241, 234, 255)),
        ("StonePi's sidekick", font_sub, PANEL * 0.405, (168, 152, 124, 255)),
    ):
        bbox = draw.textbbox((0, 0), text, font=font)
        tw = bbox[2] - bbox[0]
        draw.text(((PANEL - tw) / 2, y), text, font=font, fill=fill)

    face = int(PANEL * 0.44)
    buddy = soft_circle(
        cover_square(ART / "pebble-face-wave.jpg", face, focus_y=0.38, zoom=1.02),
        solid=0.90,
        fade=0.995,
    )
    plate.alpha_composite(buddy, ((PANEL - face) // 2, int(PANEL * 0.48)))
    return soft_circle(plate.convert("RGB").convert("RGBA"), solid=0.988, fade=1.0)


def mount_on_chassis(splash: Image.Image) -> Image.Image:
    chassis = Image.open(ART / "device-chassis.png").convert("RGBA")
    cw, ch = chassis.size
    alpha = Image.new("L", (cw, ch), 0)
    ImageDraw.Draw(alpha).ellipse((1, 1, cw - 2, ch - 2), fill=255)
    alpha = alpha.filter(ImageFilter.GaussianBlur(1.0))
    chassis.putalpha(alpha)

    inset = int(round(cw * 0.055))
    sw = cw - 2 * inset
    screen = splash.resize((sw, sw), Image.Resampling.LANCZOS)
    smask = Image.new("L", (sw, sw), 0)
    ImageDraw.Draw(smask).ellipse((0, 0, sw - 1, sw - 1), fill=255)
    smask = smask.filter(ImageFilter.GaussianBlur(0.6))

    punch = Image.new("L", (cw, ch), 0)
    ImageDraw.Draw(punch).ellipse((inset, inset, inset + sw - 1, inset + sw - 1), fill=255)
    punch = punch.filter(ImageFilter.GaussianBlur(0.6))
    chassis.putalpha(ImageChops.subtract(chassis.getchannel("A"), punch))

    layer = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    layer.paste(screen, (inset, inset), smask)
    return Image.alpha_composite(chassis, layer)


def hero_canvas(device: Image.Image, W: int = 1400, H: int = 780) -> Image.Image:
    canvas = Image.new("RGB", (W, H), PAPER)
    px = canvas.load()
    wx, wy, r_max = W * 0.70, H * 0.18, W * 0.55
    for y in range(H):
        for x in range(W):
            d = ((x - wx) ** 2 + (y - wy) ** 2) ** 0.5 / r_max
            if d >= 1.0:
                continue
            t = d * d
            px[x, y] = (
                int(WASH[0] + (PAPER[0] - WASH[0]) * t),
                int(WASH[1] + (PAPER[1] - WASH[1]) * t),
                int(WASH[2] + (PAPER[2] - WASH[2]) * t),
            )

    target_h = int(H * 0.86)
    scale = target_h / device.height
    nw, nh = int(device.width * scale), int(device.height * scale)
    device_s = device.resize((nw, nh), Image.Resampling.LANCZOS)
    sx, sy = (W - nw) // 2, (H - nh) // 2

    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    blob = Image.new("L", (nw, nh), 0)
    ImageDraw.Draw(blob).ellipse((12, 12, nw - 13, nh - 13), fill=150)
    blob = blob.filter(ImageFilter.GaussianBlur(42))
    shadow.paste((22, 25, 27, 52), (sx, sy + 26), blob)

    base = Image.alpha_composite(canvas.convert("RGBA"), shadow)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    layer.paste(device_s, (sx, sy), device_s)
    return Image.alpha_composite(base, layer).convert("RGB")


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    device = mount_on_chassis(compose_splash())
    hero = hero_canvas(device)
    hero.save(OUT, "PNG", optimize=True)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes) {hero.size}")


if __name__ == "__main__":
    main()
