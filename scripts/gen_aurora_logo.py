#!/usr/bin/env python3
"""Generate Aurora TV deploy and in-app assets from official branding sources."""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
BRANDING = ROOT / "src" / "app" / "res" / "branding"
SPLASH_SOURCE = BRANDING / "aurora_splash.png"
ICON_SOURCE = BRANDING / "aurora_icon.png"

# The fork installs next to upstream Aurora and gets its own home-screen tile,
# so the icon has to say which one it is. The arch leaves a dark band below its
# feet; the mark sits there rather than on top of any artwork. Sized off the
# tile, not the canvas, so it survives the 130 px webOS icon.
DS5_MARK_TEXT = "DS5"
DS5_MARK_FONT = Path("/usr/share/fonts/truetype/lato/Lato-Black.ttf")
DS5_MARK_CAP_FRACTION = 0.098  # of tile height
DS5_MARK_TRACKING = 0.14  # em
DS5_MARK_BASELINE = 0.90  # fraction of tile height, from the tile top
DS5_MARK_FILL = (234, 240, 255, 250)
DS5_MARK_GLOW = (120, 190, 255, 90)


def tile_bounds(im: Image.Image) -> tuple[int, int, int, int]:
    """Bounding box of the rounded tile inside the transparent/black canvas."""
    rgb = im.convert("RGB")
    # The tile body is a very dark navy but never pure black; the canvas is.
    mask = rgb.point(lambda v: 255 if v > 4 else 0).convert("L")
    box = mask.getbbox()
    if box is None:
        return 0, 0, im.width, im.height
    return box


def draw_ds5_mark(icon: Image.Image) -> Image.Image:
    """Stamp a small DS5 wordmark into the dark band below the arch."""
    out = icon.convert("RGBA")
    left, top, right, bottom = tile_bounds(out)
    tile_w = right - left
    tile_h = bottom - top

    cap = max(1, int(round(tile_h * DS5_MARK_CAP_FRACTION)))
    if not DS5_MARK_FONT.is_file():
        raise SystemExit(f"Missing font for the DS5 mark: {DS5_MARK_FONT}")
    # Ask for a size whose cap height matches the target rather than trusting
    # the nominal point size, which includes ascender and descender slack.
    probe = ImageFont.truetype(str(DS5_MARK_FONT), cap)
    probe_cap = probe.getbbox("D")[3] - probe.getbbox("D")[1]
    font = ImageFont.truetype(str(DS5_MARK_FONT), max(1, int(round(cap * cap / probe_cap))))

    tracking = int(round(font.size * DS5_MARK_TRACKING))
    widths = [font.getbbox(ch)[2] - font.getbbox(ch)[0] for ch in DS5_MARK_TEXT]
    total = sum(widths) + tracking * (len(DS5_MARK_TEXT) - 1)

    layer = Image.new("RGBA", out.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    baseline = top + int(round(tile_h * DS5_MARK_BASELINE))
    x = left + (tile_w - total) // 2
    for ch, w in zip(DS5_MARK_TEXT, widths):
        box = font.getbbox(ch)
        draw.text((x - box[0], baseline - box[3]), ch, font=font, fill=DS5_MARK_FILL)
        x += w + tracking

    # A soft cool glow echoes the arch's light and keeps the mark from reading
    # as a sticker pasted on black.
    glow = Image.new("RGBA", out.size, (0, 0, 0, 0))
    glow.paste(Image.new("RGBA", out.size, DS5_MARK_GLOW), (0, 0), layer)
    glow = glow.filter(ImageFilter.GaussianBlur(max(1, font.size // 6)))

    out.alpha_composite(glow)
    out.alpha_composite(layer)
    return out


def write_res_header(png: Path, header: Path, symbol: str) -> None:
    """Re-emit the checked-in byte array the app compiles the image into.

    src/app/res/gen/*.h is committed, not built, so regenerating a PNG without
    regenerating its header silently leaves the old image in the binary while
    every file on disk looks updated.
    """
    data = png.read_bytes()
    lines = [
        "  " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ","
        for i in range(0, len(data), 16)
    ]
    header.write_text(
        "#pragma once\n"
        f"const unsigned char res_{symbol}_data[] = {{\n"
        + "\n".join(lines)
        + "\n};\n"
        f"extern const unsigned char res_{symbol}_data[];\n"
        f"#define res_{symbol}_size {len(data)}\n"
    )


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
    icon_src = draw_ds5_mark(Image.open(ICON_SOURCE))

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
    out_gen = ROOT / "src" / "app" / "res" / "gen"
    write_res_header(out_img / "moonlight.png", out_gen / "moonlight.h", "moonlight")
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
