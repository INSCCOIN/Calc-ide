#!/usr/bin/env python3
"""calcxfer.py - copy files to and from a PicoCalc running CalcIDE, over USB.

On the PicoCalc type `xfer`, then on the PC:

    python3 calcxfer.py PORT ls [DIR]
    python3 calcxfer.py PORT get REMOTE [LOCAL]
    python3 calcxfer.py PORT put LOCAL [REMOTE]     (REMOTE may be a directory ending in /)
    python3 calcxfer.py PORT mkdir REMOTE
    python3 calcxfer.py PORT rm REMOTE
    python3 calcxfer.py PORT bye                    (CalcIDE goes back to the prompt)

PORT is /dev/ttyACM0 on Linux, /dev/cu.usbmodem... on macOS, or COM3 etc.
on Windows. pyserial is used if it is installed (it is needed on Windows);
otherwise the port is opened directly (Linux and macOS).
Several put/get commands can be chained: `calcxfer.py PORT put a.c put b.c`.
"""
import os
import sys
import time
import zlib

BLOCK = 1024


class Port:
    def __init__(self, name=None, fd=None):
        self.ser = None
        self.fd = fd
        if fd is not None:
            return
        try:
            import serial  # pyserial
            self.ser = serial.Serial(name, 115200, timeout=10)
        except ImportError:
            import termios
            import tty
            self.fd = os.open(name, os.O_RDWR | os.O_NOCTTY)
            tty.setraw(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[3] &= ~termios.ECHO
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def write(self, data):
        if self.ser:
            self.ser.write(data)
            self.ser.flush()
        else:
            while data:
                n = os.write(self.fd, data)
                data = data[n:]

    def read(self, n, timeout=10.0):
        out = b""
        end = time.time() + timeout
        while len(out) < n:
            if self.ser:
                chunk = self.ser.read(n - len(out))
            else:
                import select
                r, _, _ = select.select([self.fd], [], [], max(0.0, end - time.time()))
                chunk = os.read(self.fd, n - len(out)) if r else b""
            if not chunk and time.time() > end:
                raise IOError("timed out waiting for the PicoCalc (is `xfer` running?)")
            out += chunk
        return out

    def line(self, timeout=10.0):
        out = b""
        while not out.endswith(b"\n"):
            out += self.read(1, timeout)
        return out.decode("utf-8", "replace").rstrip("\r\n")


class Calc:
    def __init__(self, port):
        self.p = port

    def cmd(self, text):
        self.p.write((text + "\n").encode())
        reply = self.p.line()
        if reply.startswith("ERR"):
            raise IOError(reply[4:])
        return reply

    def hello(self):
        return self.cmd("HELLO")

    def ls(self, path="/"):
        first = self.cmd("LS " + path)
        out = []
        line = first
        while line != "END":
            kind, size, name = line.split(" ", 2)
            out.append((kind == "D", int(size), name))
            line = self.p.line()
        return out

    def get(self, remote):
        size = int(self.cmd("GET " + remote).split()[1])
        data = self.p.read(size, timeout=10 + size / 20000)
        crc = self.p.line().split()[1]
        if int(crc, 16) != zlib.crc32(data) & 0xFFFFFFFF:
            raise IOError("checksum mismatch reading " + remote)
        return data

    def put(self, remote, data, progress=None):
        self.cmd("PUT %s %d" % (remote, len(data)))
        for i in range(0, len(data), BLOCK):
            self.p.write(data[i:i + BLOCK])
            ack = self.p.line()
            if ack != "A":
                raise IOError(ack)
            if progress:
                progress(min(i + BLOCK, len(data)), len(data))
        final = self.p.line()
        if final.startswith("ERR"):
            raise IOError(final[4:])
        if int(final.split()[1], 16) != zlib.crc32(data) & 0xFFFFFFFF:
            raise IOError("checksum mismatch writing " + remote)

    def mkdir(self, remote):
        self.cmd("MKDIR " + remote)

    def rm(self, remote):
        self.cmd("RM " + remote)

    def bye(self):
        self.cmd("BYE")


def bar(done, total):
    w = 30
    f = int(w * done / total) if total else w
    sys.stdout.write("\r  [%s%s] %d/%d" % ("#" * f, "." * (w - f), done, total))
    sys.stdout.flush()


def run(calc, args):
    calc.hello()
    i = 0
    while i < len(args):
        op = args[i]
        a = args[i + 1:]
        if op == "ls":
            path = a[0] if a and a[0] not in OPS else "/"
            for isdir, size, name in calc.ls(path):
                print("%-28s %s" % (name + ("/" if isdir else ""), "<dir>" if isdir else size))
            i += 2 if a and a[0] not in OPS else 1
        elif op == "get":
            remote = a[0]
            local = a[1] if len(a) > 1 and a[1] not in OPS else os.path.basename(remote)
            data = calc.get(remote)
            with open(local, "wb") as f:
                f.write(data)
            print("got %s -> %s (%d bytes)" % (remote, local, len(data)))
            i += 3 if local is not None and len(a) > 1 and a[1] not in OPS else 2
        elif op == "put":
            local = a[0]
            remote = a[1] if len(a) > 1 and a[1] not in OPS else "/" + os.path.basename(local)
            if remote.endswith("/"):
                remote += os.path.basename(local)
            with open(local, "rb") as f:
                data = f.read()
            print("put %s -> %s" % (local, remote))
            calc.put(remote, data, bar if sys.stdout.isatty() else None)
            if sys.stdout.isatty():
                print()
            i += 3 if len(a) > 1 and a[1] not in OPS else 2
        elif op in ("mkdir", "rm"):
            getattr(calc, op)(a[0])
            print("%s %s" % (op, a[0]))
            i += 2
        elif op == "bye":
            calc.bye()
            i += 1
        else:
            raise SystemExit("unknown command: " + op)


OPS = ("ls", "get", "put", "mkdir", "rm", "bye")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    try:
        run(Calc(Port(sys.argv[1])), sys.argv[2:])
    except (IOError, OSError) as e:
        sys.exit("calcxfer: %s" % e)


if __name__ == "__main__":
    main()
