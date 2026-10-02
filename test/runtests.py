#!/usr/bin/env python3
"""
FATNT user-mode test runner.

Builds the driver sources with the fake NT kernel (kern.c) and runs them
against real FAT12/16/32 images, then checks each image with fsck.fat.
Needs: gcc, dosfstools (mkfs.fat, fsck.fat), mtools (mcopy).
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")

def run(cmd, **kw):
    return subprocess.run(cmd, **kw)

def build(tmp):
    exe = os.path.join(tmp, "fattest")
    srcs = [os.path.join(SRC, f) for f in os.listdir(SRC) if f.startswith("fat") and f.endswith(".c")]
    cmd = ["gcc", "-g", "-w", "-I", SRC, "-I", HERE, "-o", exe,
           os.path.join(HERE, "fattest.c"), os.path.join(HERE, "kern.c")] + srcs
    if run(cmd).returncode != 0:
        print("BUILD FAILED"); sys.exit(1)
    return exe

def seed(img):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    hello = os.path.join(os.path.dirname(img), "hello.txt")
    ln = os.path.join(os.path.dirname(img), "ln.txt")
    open(hello, "w").write("hello, fatnt\n")
    open(ln, "w").write("long name content\n")
    run(["mcopy", "-i", img, hello, "::HELLO.TXT"], env=env)
    run(["mcopy", "-i", img, ln, "::A Long File Name.txt"], env=env)

def main():
    tmp = tempfile.mkdtemp(prefix="fatnt-")
    exe = build(tmp)
    cases = [("12", 4096), ("16", 131072), ("32", 1048576)]
    rc = 0
    for typ, sectors in cases:
        img = os.path.join(tmp, "fat%s.img" % typ)
        with open(img, "wb") as f:
            f.truncate(sectors * 512)
        fs = "12" if typ == "12" else ("16" if typ == "16" else "32")
        run(["mkfs.fat", "-F", fs, img], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        seed(img)
        print("=" * 40, "FAT%s" % typ)
        r = run([exe, img, "FAT%s" % typ])
        if r.returncode != 0:
            rc = 1
        chk = run(["fsck.fat", "-n", img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = chk.stdout.decode(errors="replace")
        bad = any(w in out for w in ("Dirty", "error", "Error", "orphan", "Bad"))
        print(out.strip().splitlines()[-1] if out.strip() else "(fsck no output)")
        if bad:
            print("fsck reported issues"); rc = 1
    print("\nALL PASS" if rc == 0 else "\nFAILURES")
    sys.exit(rc)

if __name__ == "__main__":
    main()
