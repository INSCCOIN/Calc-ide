#!/usr/bin/env python3
"""Generate platform/picocalc/fonts.c: the two console fonts.

  8x10 (40 columns): the IBM PC BIOS-style 8x8 glyphs of font8x8 by Daniel
        Hepper (public domain), centred in a 10-pixel cell - the MS-DOS look.
  5x10 (64 columns): the X11 misc-fixed 5x8 font by Markus Kuhn (public
        domain), centred in a 10-pixel cell.

Both cover ASCII 32-126 plus the CP437 shade, box-drawing and block
characters 0xB0-0xDF, so programs can draw DOS-style frames.
Bit 0 of each row byte is the leftmost pixel.

usage: make_fonts.py FONT8X8_DIR 5x8.bdf > platform/picocalc/fonts.c
"""
import re, sys, os

CP437_HI = [0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
            0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
            0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580]
EXTRA = {0xF8: 0x00B0, 0xFA: 0x00B7, 0xFE: 0x25A0, 0xF1: 0x00B1, 0xFD: 0x00B2, 0xE6: 0x00B5, 0x9C: 0x00A3, 0xF6: 0x00F7}

def load_font8x8(d):
    glyphs = {}
    for fn, base in (("font8x8_basic.h", 0), ("font8x8_box.h", 0x2500), ("font8x8_block.h", 0x2580), ("font8x8_ext_latin.h", 0xA0)):
        txt = open(os.path.join(d, fn)).read()
        rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,?\s*){8})\}", txt)
        for i, r in enumerate(rows):
            glyphs[base + i] = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", r)]
    # U+25A0 black square, for CP437 0xFE
    glyphs.setdefault(0x25A0, [0, 0, 0x3C, 0x3C, 0x3C, 0x3C, 0, 0])
    return glyphs

def load_bdf(path):
    glyphs = {}
    enc = None; bbx = None; bitmap = None
    for line in open(path, encoding="latin-1"):
        p = line.split()
        if not p: continue
        if p[0] == "ENCODING": enc = int(p[1])
        elif p[0] == "BBX": bbx = list(map(int, p[1:5]))
        elif p[0] == "BITMAP": bitmap = []
        elif p[0] == "ENDCHAR":
            w, h, xo, yo = bbx
            rows = [0] * 8
            # the font box is 5x8 with the baseline one row above the bottom
            top = 8 - 1 - (yo + h - 1) - 1 + 1
            top = (7 - yo - h)
            for i, v in enumerate(bitmap):
                bits = int(v, 16) << (8 * ((len(v) + 1) // 2) - 8)
                y = top + i
                if 0 <= y < 8:
                    b = 0
                    for x in range(8):
                        if bits & (0x80 >> x) and 0 <= x + xo < 5: b |= 1 << (x + xo)
                    rows[y] = b
            glyphs[enc] = rows
            bitmap = None
        elif bitmap is not None:
            bitmap.append(p[0])
    return glyphs

def cell(g8, code, uni, is_box):
    """an 8-row glyph in a 10-row cell: one blank row above and below,
    except box and block characters, which repeat their edge rows so that
    lines join up across cells"""
    if g8 is None: return [0] * 10
    if is_box: return [g8[0]] + g8 + [g8[7]]
    return [0] + g8 + [0]

# CP437 box characters as (up, down, left, right): 0 none, 1 single, 2 double
BOX = {0xB3:(1,1,0,0),0xB4:(1,1,1,0),0xB5:(1,1,2,0),0xB6:(2,2,1,0),0xB7:(0,2,1,0),0xB8:(0,1,2,0),
       0xB9:(2,2,2,0),0xBA:(2,2,0,0),0xBB:(0,2,2,0),0xBC:(2,0,2,0),0xBD:(2,0,1,0),0xBE:(1,0,2,0),
       0xBF:(0,1,1,0),0xC0:(1,0,0,1),0xC1:(1,0,1,1),0xC2:(0,1,1,1),0xC3:(1,1,0,1),0xC4:(0,0,1,1),
       0xC5:(1,1,1,1),0xC6:(1,1,0,2),0xC7:(2,2,0,1),0xC8:(2,0,0,2),0xC9:(0,2,0,2),0xCA:(2,0,2,2),
       0xCB:(0,2,2,2),0xCC:(2,2,0,2),0xCD:(0,0,2,2),0xCE:(2,2,2,2),0xCF:(1,0,2,2),0xD0:(2,0,1,1),
       0xD1:(0,1,2,2),0xD2:(0,2,1,1),0xD3:(2,0,0,1),0xD4:(1,0,0,2),0xD5:(0,1,0,2),0xD6:(0,2,0,1),
       0xD7:(2,2,1,1),0xD8:(1,1,2,2),0xD9:(1,0,1,0),0xDA:(0,1,0,1)}

def synth(code, W, H=10):
    """draw a CP437 shade, box or block character in a W x H cell"""
    rows = [0] * H
    cx, cy = W // 2, H // 2 - 1
    def px(x, y):
        if 0 <= x < W and 0 <= y < H: rows[y] |= 1 << x
    if code in (0xB0, 0xB1, 0xB2):
        for y in range(H):
            for x in range(W):
                k = (x + y) % 2 if code == 0xB1 else (x % 2 == 0 and y % 2 == 0) if code == 0xB0 else not (x % 2 == 0 and y % 2 == 0)
                if k: px(x, y)
        return rows
    if code == 0xDB: return [(1 << W) - 1] * H
    if code == 0xDC: return [0] * (H // 2) + [(1 << W) - 1] * (H - H // 2)
    if code == 0xDF: return [(1 << W) - 1] * (H // 2) + [0] * (H - H // 2)
    if code == 0xDD: return [(1 << ((W + 1) // 2)) - 1] * H
    if code == 0xDE: return [((1 << W) - 1) & ~((1 << (W // 2)) - 1)] * H
    u, d, l, r = BOX[code]
    def vline(kind, y0, y1):
        for y in range(y0, y1 + 1):
            if kind == 1: px(cx, y)
            elif kind == 2: px(cx - 1, y); px(cx + 1, y)
    def hline(kind, x0, x1):
        for x in range(x0, x1 + 1):
            if kind == 1: px(x, cy)
            elif kind == 2: px(x, cy - 1); px(x, cy + 1)
    vu = cy + (1 if (l == 2 or r == 2) and u else 0)
    vd = cy - (1 if (l == 2 or r == 2) and d else 0)
    if u: vline(u, 0, vu if u == 1 else cy + (1 if not d else 0) if (l or r) else cy)
    if d: vline(d, vd if d == 1 else cy - (1 if not u else 0) if (l or r) else cy, H - 1)
    hl = cx + (1 if (u == 2 or d == 2) and l else 0)
    hr = cx - (1 if (u == 2 or d == 2) and r else 0)
    if l: hline(l, 0, hl if l == 1 else cx + (1 if not r else 0) if (u or d) else cx)
    if r: hline(r, hr if r == 1 else cx - (1 if not l else 0) if (u or d) else cx, W - 1)
    return rows

def table(name, glyphs):
    out = ["const unsigned char %s[256][10] = {" % name]
    for code in range(256):
        if 32 <= code < 127: uni = code
        elif 0xB0 <= code <= 0xDF: uni = CP437_HI[code - 0xB0]
        elif code in EXTRA: uni = EXTRA[code]
        else: uni = None
        g = glyphs.get(uni) if uni is not None else None
        if g is None and 0xB0 <= code <= 0xDF:
            rows = synth(code, 5 if name == "font_5x10" else 8)
        else:
            rows = cell(g, code, uni, 0xB0 <= code <= 0xDF)
        out.append("    {%s},  /* %02X */" % (",".join("0x%02x" % r for r in rows), code))
    out.append("};")
    return "\n".join(out)

f8 = load_font8x8(sys.argv[1])
f5 = load_bdf(sys.argv[2])
print("/* Generated by tools/make_fonts.py - do not edit.")
print(" * font_8x10: font8x8 by Daniel Hepper (public domain), IBM PC style.")
print(" * font_5x10: X11 misc-fixed 5x8 by Markus Kuhn (public domain).")
print(" * Bit 0 = leftmost pixel; CP437 0xB0-0xDF are shades/box/blocks. */\n")
print(table("font_8x10", f8))
print()
print(table("font_5x10", f5))
