#!/usr/bin/env python3
"""Generate Aurora TV deploy and in-app assets from official branding sources."""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
BRANDING = ROOT / "src" / "app" / "res" / "branding"
SPLASH_SOURCE = BRANDING / "aurora_splash.png"
ICON_SOURCE = BRANDING / "aurora_icon.png"


def resize_square(im: Image.Image, size: int) -> Image.Image:
    return im.convert("RGBA").resize((size, size), Image.Resampling.LANCZOS)


def make_splash(splash: Image.Image, width: int = 1920, height: int = 1080) -> Image.Image:
    canvas = Image.new("RGB", (width, height), (0, 0, 0))
    src = splash.convert("RGBA")
    scale = min(width / src.width, height / src.height) * 0.78
    nw = max(1, int(src.width * scale))
    nh = max(1, int(src.height * scale))
    resized = src.resize((nw, nh), Image.Resampling.LANCZOS)
    x = (width - nw) // 2
    y = (height - nh) // 2
    canvas.paste(resized, (x, y), resized)
    return canvas


def main() -> int:
    if not SPLASH_SOURCE.is_file():
        print(f"Missing splash image: {SPLASH_SOURCE}", file=sys.stderr)
        return 1
    if not ICON_SOURCE.is_file():
        print(f"Missing icon image: {ICON_SOURCE}", file=sys.stderr)
        return 1

    splash_src = Image.open(SPLASH_SOURCE)
    icon_src = Image.open(ICON_SOURCE)

    icon_96 = resize_square(icon_src, 96)
    icon_130 = resize_square(icon_src, 130)
    icon_512 = resize_square(icon_src, 512)
    splash = make_splash(splash_src)

    out_img = ROOT / "src" / "app" / "res" / "img"
    out_webos = ROOT / "deploy" / "webos"
    out_linux = ROOT / "deploy" / "linux"
    out_steam = ROOT / "deploy" / "steamlink"
    out_img.mkdir(parents=True, exist_ok=True)
    out_webos.mkdir(parents=True, exist_ok=True)
    out_linux.mkdir(parents=True, exist_ok=True)
    out_steam.mkdir(parents=True, exist_ok=True)

    icon_96.save(out_img / "moonlight.png", optimize=True)
    wordmark_legacy = out_img / "aurora_wordmark.png"
    if wordmark_legacy.is_file():
        wordmark_legacy.unlink()

    icon_130.save(out_webos / "icon.png", optimize=True)
    icon_512.save(out_webos / "icon_large.png", optimize=True)
    splash.save(out_webos / "splash.png", optimize=True)
    icon_512.save(out_linux / "moonlight-tv.png", optimize=True)
    icon_512.save(out_steam / "moonlight.png", optimize=True)

    print(f"Sources: {SPLASH_SOURCE.name} {splash_src.size}, {ICON_SOURCE.name} {icon_src.size}")
    print(f"Wrote {out_img / 'moonlight.png'} ({icon_96.size[0]}x{icon_96.size[1]})")
    print(f"Wrote {out_webos / 'icon.png'}")
    print(f"Wrote {out_webos / 'icon_large.png'}")
    print(f"Wrote {out_webos / 'splash.png'}")
    print(f"Wrote {out_linux / 'moonlight-tv.png'}")
    print(f"Wrote {out_steam / 'moonlight.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
