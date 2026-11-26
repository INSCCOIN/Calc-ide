"""Run `xfer` in the host build over a pty and drive it with calcxfer.py."""
import os, subprocess, sys, tempfile, random
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import calcxfer

def main():
    root = tempfile.mkdtemp()
    master, slave = os.openpty()
    env = dict(os.environ, CALCIDE_BATCH="1", CALCIDE_ROOT=root, CALCIDE_USB=os.ttyname(slave))
    p = subprocess.Popen([os.path.join(ROOT, "build-host", "calcide")], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    p.stdin.write(b"xfer\necho back\n"); p.stdin.flush()
    import termios, tty
    tty.setraw(master)
    c = calcxfer.Calc(calcxfer.Port(fd=master))
    assert c.hello().startswith("OK CalcIDE"), "hello"
    blob = bytes(random.Random(1).randrange(256) for _ in range(5000)) + b"\n\r\x00\xff" * 10
    c.mkdir("/up")
    c.put("/up/blob.bin", blob)
    c.put("/up/empty.txt", b"")
    assert c.get("/up/blob.bin") == blob, "round trip"
    assert c.get("/up/empty.txt") == b"", "empty"
    names = sorted(n for _, _, n in c.ls("/up"))
    assert names == ["blob.bin", "empty.txt"], names
    try:
        c.get("/nope"); assert False
    except IOError:
        pass
    c.rm("/up/empty.txt")
    assert open(os.path.join(root, "up", "blob.bin"), "rb").read() == blob
    c.bye()
    out = p.communicate(timeout=20)[0].decode()
    assert "back" in out and "transfer is on" in out and "transfer is off" in out, out
    print("ok")

main()
