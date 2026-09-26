#!/usr/bin/env python3
"""
Build iconfonts/fa-brands-400.ttf, the source the `iconfont` gulp task subsets
into gen/fa_brands_400_ttf.h -- the same role MaterialIcons-Regular.ttf plays
for the Material glyphs.

One-off, and not part of the build: run it again only to change which brand
glyphs the app carries (keep fa-brands-400.list and .codepoints in step).

    pip install fonttools brotli
    python3 fa_brands_source.py <@fortawesome/fontawesome-free>/webfonts/fa-brands-400.woff2 \
        ../iconfonts/fa-brands-400.ttf

What it does to Font Awesome Free's brands face, and why:

  - Keeps only the glyphs named in fa-brands-400.codepoints (+ .notdef).
  - Converts the CFF outlines to TrueType quadratics, so the file is a plain
    glyf font like the Material one and does not depend on the TV's FreeType
    carrying a CFF driver.
  - Moves every outline up by the face's descent and makes the em box
    0..unitsPerEm, sitting on the baseline -- the metrics Material Icons has.
    The two faces are drawn as one fontset whose line box is Material's (it is
    the primary font), and LVGL clips a label to that box: left at Font
    Awesome's own baseline, a brand glyph would hang an eighth of an em below
    it, cut off and sitting lower than the Material icon next to it.
  - Renames the family. The SIL OFL 1.1 reserves the name "Font Awesome", and
    a subset with moved outlines is a Modified Version. The copyright line is
    kept, and the licence is named in the font itself (name IDs 13/14, which
    gulp-subset-font.ts keeps in the build's re-subset too); the full text is
    iconfonts/fa-brands-400.LICENSE.txt, installed into the IPK's licenses/.
"""

import os
import sys

from fontTools import subset
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont, newTable

FAMILY = "Aurora Brands"
PS_NAME = "AuroraBrands-Regular"


def codepoints(path):
    with open(path, encoding="utf-8") as fp:
        return [int(line.split()[1], 16) for line in fp if line.strip()]


def to_glyf(font, shift_y):
    """CFF -> glyf, every outline translated by @shift_y font units."""
    order = font.getGlyphOrder()
    glyph_set = font.getGlyphSet()
    glyphs = {}
    for name in order:
        pen = TTGlyphPen(glyph_set)
        glyph_set[name].draw(TransformPen(Cu2QuPen(pen, max_err=1.0, reverse_direction=True),
                                          (1, 0, 0, 1, 0, shift_y)))
        glyphs[name] = pen.glyph()
    glyf = font["glyf"] = newTable("glyf")
    glyf.glyphOrder = order
    glyf.glyphs = glyphs
    font["loca"] = newTable("loca")
    del font["CFF "]
    maxp = font["maxp"]
    maxp.tableVersion = 0x00010000
    for attr in ("maxZones", "maxTwilightPoints", "maxStorage", "maxFunctionDefs",
                 "maxInstructionDefs", "maxStackElements", "maxSizeOfInstructions",
                 "maxComponentElements", "maxComponentDepth"):
        setattr(maxp, attr, 0)
    maxp.maxZones = 1
    post = font["post"]
    post.formatType = 2.0
    post.extraNames = []
    post.mapping = {}
    post.glyphOrder = order
    font["head"].glyphDataFormat = 0
    font.sfntVersion = "\x00\x01\x00\x00"
    # Loaded from WOFF2, which TTFont would otherwise write back out.
    font.flavor = None


def main(src, dst):
    wanted = codepoints(os.path.splitext(dst)[0] + ".codepoints")

    font = TTFont(src)
    options = subset.Options()
    options.name_IDs = [0, 5]
    options.layout_features = []
    options.hinting = False
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=wanted)
    subsetter.subset(font)

    upem = font["head"].unitsPerEm
    descent = -font["hhea"].descent
    to_glyf(font, descent)

    hhea = font["hhea"]
    hhea.ascent, hhea.descent, hhea.lineGap = upem, 0, 0
    os2 = font["OS/2"]
    os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap = upem, 0, 0
    os2.usWinAscent, os2.usWinDescent = upem, 0

    name = font["name"]
    for name_id, text in (
            (1, FAMILY),
            (2, "Regular"),
            (3, PS_NAME),
            (4, FAMILY + " Regular"),
            (6, PS_NAME),
            (10, "Subset of Font Awesome Free 7.3.1 Brands, outlines converted to TrueType "
                 "and moved onto the baseline"),
            (13, "This Font Software is licensed under the SIL Open Font License, Version 1.1."),
            (14, "https://openfontlicense.org"),
    ):
        name.setName(text, name_id, 3, 1, 0x409)

    missing = [cp for cp in wanted if cp not in font.getBestCmap()]
    if missing:
        sys.exit("not in the source font: " + ", ".join("U+%04X" % cp for cp in missing))
    font.save(dst)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
