#!/usr/bin/env python3
"""Check a .song file: every channel parses, and all channels have the same
length (in beats). usage: songcheck.py FILE.song..."""
import re, sys
def expand(s):
    while True:
        m = re.search(r"\[([^\[\]]*)\](?:[xX*](\d+))?", s)
        if not m: return s
        s = s[:m.start()] + (" " + m.group(1) + " ") * int(m.group(2) or 2) + s[m.end():]
def beats(text):
    deflen, total = 4, 0.0
    for tok in expand(text).replace(",", " ").split():
        t = tok.upper()
        if t.startswith("I") or re.fullmatch(r"[TOV]\d+", t): continue
        if re.fullmatch(r"L\d+", t): deflen = int(t[1:]); continue
        m = re.fullmatch(r"(SN|HH|OH|CP|TH|TL|CY|K|R|[A-G](?:#|B)?\d?)(?:/(\d+))?(\.)?", t)
        if not m: raise SystemExit("bad token %r" % tok)
        n = int(m.group(2) or deflen)
        d = 4.0 / n * (1.5 if m.group(3) else 1)
        total += d
    return total
bad = 0
for f in sys.argv[1:]:
    chans = {}
    for line in open(f):
        line = re.split(r"(?:^|(?<=\s))#", line)[0].strip()
        if not line: continue
        ch = "1"
        if re.match(r"[1-4]:", line): ch, line = line[0], line[2:]
        chans[ch] = chans.get(ch, "") + " " + line
    lens = {c: beats(t) for c, t in sorted(chans.items())}
    ok = len(set(round(v, 3) for v in lens.values())) == 1
    bad += not ok
    print("%-16s %s %s" % (f, "ok " if ok else "MISMATCH", " ".join("%s:%g" % kv for kv in lens.items())))
sys.exit(1 if bad else 0)
