#!/usr/bin/env python3
"""Render a console grid dumped by the host build (CALCIDE_GRID=file) into a
PNG, using the firmware's own font tables and palette.
usage: screenshot.py GRID OUT.png [scale]"""
import re, sys, os
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, "..", "platform", "picocalc", "fonts.c")).read()
def table(name):
    blk = src[src.index("const unsigned char %s" % name):]
    rows = re.findall(r"\{(0x[^}]*)\},\s*/\* .. \*/", blk)[:256]
    return [[int(v, 16) for v in r.split(",")] for r in rows]
f8, f5 = table("font_8x10"), table("font_5x10")
con = open(os.path.join(here, "..", "src", "console.c")).read()
pal565 = [int(v, 16) for v in re.findall(r"0x[0-9A-F]{4}", con[con.index("con_palette[16]"):])[:16]]
pal = [(((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in pal565]
lines = open(sys.argv[1]).read().split("\n")
cols = int(lines[0])
font, cw = (f5, 5) if cols == 64 else (f8, 8)
img = Image.new("RGB", (320, 320))
px = img.load()
for r in range(32):
    cells = [int(v, 16) for v in lines[1 + r].split()]
    for c, cell in enumerate(cells):
        ch, a = cell & 255, cell >> 8
        fg, bg = pal[a & 15], pal[(a >> 4) & 15]
        g = font[ch]
        for y in range(10):
            for x in range(cw):
                px[c * cw + x, r * 10 + y] = fg if g[y] >> x & 1 else bg
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 2
img = img.resize((320 * scale, 320 * scale), Image.NEAREST)
img.save(sys.argv[2])
