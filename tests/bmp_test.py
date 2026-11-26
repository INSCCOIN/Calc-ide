#!/usr/bin/env python3
"""Decode BMP files of every supported depth with the firmware's splash code
(host build) and compare the framebuffer with what PIL reads."""
import os, sys, subprocess, tempfile, struct
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDE = os.path.join(ROOT, "build-host", "calcide")

def make_image(w, h):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255) // max(w - 1, 1), (y * 255) // max(h - 1, 1), ((x ^ y) * 7) & 255)
    return im

def rgb565(c):
    r, g, b = c[:3]
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def check(work, name, im, save, w, h):
    path = os.path.join(work, name)
    save(path)
    ref = Image.open(path).convert("RGB")
    fbf = os.path.join(work, "fb.raw")
    env = dict(os.environ, CALCIDE_ROOT=work, CALCIDE_BATCH="1", CALCIDE_SPLASH="1", CALCIDE_FB=fbf,
               CALCIDE_PF=os.path.join(work, "pf.bin"))
    open(os.path.join(work, "CalcIDE.conf"), "w").write("splash_image = /%s\nsplash_ms = 100\n" % name)
    subprocess.run([IDE], input=b"", env=env, capture_output=True, timeout=30)
    fb = struct.unpack("<%dH" % (320 * 320), open(fbf, "rb").read())
    vw, vh = min(w, 320), min(h, 320)
    sx0, sy0, x0, y0 = (w - vw) // 2, (h - vh) // 2, (320 - vw) // 2, (320 - vh) // 2
    bad = 0
    for y in range(0, vh, 7):
        for x in range(0, vw, 5):
            if fb[(y0 + y) * 320 + x0 + x] != rgb565(ref.getpixel((sx0 + x, sy0 + y))):
                bad += 1
    return bad

work = tempfile.mkdtemp()
fails = 0
im = make_image(200, 150)
cases = [
    ("rgb24.bmp", lambda p: im.save(p), 200, 150),
    ("rgb32.bmp", lambda p: im.convert("RGBA").save(p), 200, 150),
    ("pal8.bmp", lambda p: im.convert("P", palette=Image.ADAPTIVE, colors=256).save(p), 200, 150),
    ("pal4.bmp", lambda p: im.convert("P", palette=Image.ADAPTIVE, colors=16).save(p), 200, 150),
    ("mono.bmp", lambda p: im.convert("1").save(p), 200, 150),
    ("big24.bmp", lambda p: make_image(500, 400).save(p), 500, 400),
]
for name, save, w, h in cases:
    bad = check(work, name, im, save, w, h)
    print(("PASS" if bad == 0 else "FAIL") + " bmp/%s (%d mismatches)" % (name, bad))
    fails += bad != 0
sys.exit(1 if fails else 0)
