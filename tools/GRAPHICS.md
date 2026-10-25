# Pebble graphics

Same rules as Muse gadgets on the round 466 px AMOLED:

| Do | Don’t |
| --- | --- |
| Bake at **466×466** plate | Runtime shrink of dithered art (soft) |
| Buddy ~**55%** of disc (`FACE=256`) on dark field | Full-bleed character filling the circle |
| `LANCZOS` resize, supersample vectors | Jaggy low-res polygons |
| **Bayer dither** into RGB565 | Flat 565 truncate (banding) |
| Soft circular vignette on buddy + bezel | Hard square art in the circle |
| Idle breathe = LVGL translate on the plate | Baking motion into huge plates |

Source art lives in `tools/art/`. Regenerate with:

```bash
python tools/mkassets.py
```

Then rebuild firmware or `sim/build.py`.
