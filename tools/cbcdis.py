#!/usr/bin/env python3
"""Disassemble a CalcIDE .cbc file: cbcdis.py prog.cbc"""
import sys, struct, re, os
ops = {}
for line in open(os.path.join(os.path.dirname(__file__), "..", "cc", "ops.h")):
    m = re.match(r"#define OP_(\w+)\s+(\d+)", line)
    if m: ops[int(m.group(2))] = m.group(1)
d = open(sys.argv[1], "rb").read()
h = struct.unpack("<12I", d[:48])
csz, dinit, dtot, entry = h[2], h[3], h[4], h[5]
code = d[48:48 + csz]
print("code %d data %d total %d entry %d" % (csz, dinit, dtot, entry))
fsz = {"IMM": "i", "IMM8": "b", "IMM16": "h", "LEA": "h", "LEA32": "i", "JMP": "i", "JZ": "i", "JNZ": "i",
       "CALL": "i", "CALLI": "B", "ENTER": "Bi", "MEMCPY": "i", "SYS": "HB", "INC": "Bi", "LLW": "h",
       "MEMSET": "i", "ADDI": "i", "LGW": "i"}
pc = 0
while pc < csz:
    o = code[pc]; n = ops.get(o, "?%d" % o); f = fsz.get(n, "")
    sz = struct.calcsize("<" + f) if f else 0
    args = struct.unpack("<" + f, code[pc + 1:pc + 1 + sz]) if f else ()
    print("%6d  %-8s %s" % (pc, n, " ".join(map(str, args))))
    pc += 1 + sz
data = d[48 + csz:48 + csz + dinit]
print("data:", data)
