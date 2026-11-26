#!/usr/bin/env python3
"""CalcIDE test suite (host build).

1. Every tests/c/*.c program is compiled with the CalcIDE compiler and run in
   the VM. Its output must match tests/c/NAME.expect if that file exists,
   otherwise the output of the same program compiled by the host gcc.
2. tests/err/*.c must fail with the runtime error named in their first line
   ("// error: division by zero").
3. Self-hosting: the native compiler compiles cc.c to cc1.cbc; the VM runs
   cc1.cbc to compile cc.c again; the two outputs must be byte-identical.
4. The shell is driven in batch mode through tests/shell/*.txt scripts.
"""
import os, subprocess, sys, shutil, glob, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "build-host", "cctool")
IDE = os.path.join(ROOT, "build-host", "calcide")
passed = failed = 0

def run(cmd, cwd, env_extra=None, inp=None, timeout=60):
    env = dict(os.environ)
    env["CALCIDE_ROOT"] = cwd
    env["CALCIDE_BATCH"] = "1"
    env["CALCIDE_PF"] = os.path.join(cwd, "pagefile.bin")
    if env_extra: env.update(env_extra)
    p = subprocess.run(cmd, cwd=cwd, env=env, input=inp, capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode("latin-1"), p.stderr.decode("latin-1")

def report(ok, name, detail=""):
    global passed, failed
    if ok: passed += 1; print("PASS", name)
    else:
        failed += 1; print("FAIL", name)
        if detail: print("   " + detail.replace("\n", "\n   ")[:3000])

def main():
    work = tempfile.mkdtemp(prefix="calcide-test-")
    for f in sorted(glob.glob(os.path.join(ROOT, "tests", "c", "*.c"))):
        name = os.path.basename(f)[:-2]
        shutil.copy(f, work)
        exp_file = f[:-2] + ".expect"
        if os.path.exists(exp_file):
            expected = open(exp_file).read()
        else:
            src = open(f).read().replace("\n##", "\n//")
            gsrc = os.path.join(work, name + "_gcc.c")
            open(gsrc, "w").write(src)
            r = subprocess.run(["gcc", "-w", "-fsigned-char", "-o", gsrc[:-2], gsrc, "-lm"], capture_output=True)
            if r.returncode:
                report(False, name, "gcc failed: " + r.stderr.decode()); continue
            expected = subprocess.run([gsrc[:-2]], capture_output=True, timeout=30).stdout.decode("latin-1")
        rc, out, err = run([TOOL, "cc", name + ".c"], work)
        if rc:
            report(False, name, "compile failed: " + out + err); continue
        rc, out, err = run([TOOL, "run", name + ".cbc"], work)
        report(out == expected, name, "--- expected\n" + expected + "--- got\n" + out + err)

    for f in sorted(glob.glob(os.path.join(ROOT, "tests", "err", "*.c"))):
        name = os.path.basename(f)[:-2]
        shutil.copy(f, work)
        want = open(f).readline().split("error:", 1)[1].strip()
        rc, out, err = run([TOOL, "cc", name + ".c"], work)
        if want.startswith("compile "):
            report(rc != 0 and want[8:] in out, "err/" + name, out); continue
        if rc:
            report(False, "err/" + name, "compile failed: " + out); continue
        rc, out, err = run([TOOL, "run", name + ".cbc"], work)
        report(("Runtime error: " + want) in out, "err/" + name, out)

    # self-hosting
    os.makedirs(os.path.join(work, "cc"), exist_ok=True)
    for n in ("cc.c", "ops.h", "prelude.h"):
        shutil.copy(os.path.join(ROOT, "cc", n), os.path.join(work, "cc"))
    rc1, o1, _ = run([TOOL, "cc", "cc/cc.c", "-o", "cc1.cbc"], work)
    rc2, o2, _ = run([TOOL, "run", "cc1.cbc", "cc/cc.c", "-o", "cc2.cbc"], work,
                     {"CALCIDE_RAM_KB": "4000", "CALCIDE_VMKB": "400"}, timeout=120)
    same = rc1 == 0 and rc2 == 0 and open(os.path.join(work, "cc1.cbc"), "rb").read() == open(os.path.join(work, "cc2.cbc"), "rb").read()
    report(same, "self-host (cc1 == cc2)", o1 + o2)
    # the self-hosted compiler builds and runs a test program
    if same:
        rc, out, _ = run([TOOL, "run", "cc2.cbc", "t01_basics.c", "-o", "sh.cbc"], work,
                         {"CALCIDE_RAM_KB": "4000", "CALCIDE_VMKB": "400"})
        rc, out2, _ = run([TOOL, "run", "sh.cbc"], work)
        rc, out3, _ = run([TOOL, "run", "t01_basics.cbc"], work)
        report(out2 == out3 and out2 != "", "self-hosted compiler output runs", out + out2)

    # the firmware's FatFs layer against FAT32 and FAT16 disk images
    fh = os.path.join(ROOT, "build-host", "fatfs_host")
    if os.path.exists(fh) and shutil.which("mkfs.vfat"):
        for bits, mb in (("32", "64"), ("16", "32")):
            img = os.path.join(work, "sd%s.img" % bits)
            subprocess.run(["dd", "if=/dev/zero", "of=" + img, "bs=1M", "count=" + mb], capture_output=True)
            subprocess.run(["mkfs.vfat", "-F", bits, img], capture_output=True)
            r = subprocess.run([fh, img], capture_output=True)
            out = r.stdout.decode()
            fsck = subprocess.run(["fsck.vfat", "-n", img], capture_output=True)
            report(r.returncode == 0 and "fatfs ok" in out and fsck.returncode == 0,
                   "fatfs FAT%s (and fsck clean)" % bits, out + fsck.stdout.decode())

    # the boot-screen BMP decoder against PIL
    if os.path.exists(IDE):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "bmp_test.py")], capture_output=True)
        for line in r.stdout.decode().splitlines():
            report(line.startswith("PASS"), line.split(" ", 1)[1], line)

    # shell scripts
    if os.path.exists(IDE):
        for f in sorted(glob.glob(os.path.join(ROOT, "tests", "shell", "*.txt"))):
            name = os.path.basename(f)[:-4]
            text = open(f).read()
            script, _, expected = text.partition("\n----\n")
            sw = os.path.join(work, "sh_" + name)
            os.makedirs(sw, exist_ok=True)
            rc, out, err = run([IDE], sw, {"CALCIDE_SCREEN": "1"}, inp=script.encode())
            missing = [l for l in expected.strip().splitlines() if l.strip() and l.strip() not in out]
            report(not missing, "shell/" + name, "missing: %r\n--- output\n%s" % (missing, out + err))

    print("\n%d passed, %d failed" % (passed, failed))
    shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
