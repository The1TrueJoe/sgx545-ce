"""Run hand-assembled USSE fragment programs on the EA3 and read their results back.

    python3 hwt.py <suite module> [-k substring] [--dry]

Each test is a main pixel program (asm.py syntax) whose result is the raw 32-bit
word it moves to o0: the PBE writes o0 unchanged into the BGRA8888 back buffer,
so a pixel inside the triangle *is* the register (byte 0 = B ... byte 3 = A).
A suite module defines TESTS = [(name, [insn, ...], expect), ...]; expect is an
int (the exact word), None (just record), or a callable(word) -> bool.

The frame is the probe's ref.bin replay (re/decode/mkreplay.py) with:
- the main pixel program at 0x37008388 (after its `phas.i` word at 0x380),
  ending with the test's last instruction, which must carry `end`;
- the main program's DOUTU (PDS data 0x2e018020) given 16 temporaries
  (word 0 top byte 4; the captured program had none);
- a known clear colour, so a program that never ran is obvious.
The data file is the same for every test (uploaded once); only the pokes differ.

Environment: CAP (dir with ref.bin), REPLAY (static replay binary), EA3 (ssh helper).
"""
import argparse
import importlib
import os
import shutil
import struct
import subprocess
import sys
HERE = os.path.dirname(os.path.abspath(__file__))
# re/isa/dis.py shadows the stdlib `dis` that dataclasses (pvrlog) needs: import that first
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != HERE]
import dataclasses  # noqa: E402,F401
sys.path.insert(0, HERE)
import asm  # noqa: E402
import hw  # noqa: E402

PROG = 0x37008388           # main pixel program, after its phas.i header
DOUTU0 = 0x2e018020         # main program's DOUTU word 0 in the PDS data
DOUTU0_W = 0x04010700       # code base 0, offset 0x8380, temps byte 4
FB = 0x3e265000
PITCH = 736
ROW = (279, 344, 32)        # y, first x, pixels: inside the default triangle
CLEAR = "0.2,0.4,0.6,0.8"   # background -> 0xcc336699 (ARGB)
BOX = "/tmp/hwt"


def env(k, d=None):
    v = os.environ.get(k, d)
    if v is None:
        sys.exit(f"hwt: set {k}")
    return v


def base(work):
    """the shared replay script (with {POKES} placeholders) and data file"""
    sys.path.insert(0, os.path.join(HERE, "..", "decode"))
    import mkreplay
    import pvrlog
    out = os.path.join(work, "base")
    a = argparse.Namespace(verts=None, clear=CLEAR, color=None, usse=[], poke=[],
                           vb=0x3e51d000, uniform=0x2e006184)
    gen = mkreplay.Gen(pvrlog.load(os.path.join(env("CAP"), "ref.bin")), out, set(), {1, 8, 10}, 200, 2000)
    gen.pokes = mkreplay.overrides(a) + [(DOUTU0, [DOUTU0_W], "main program: 16 temporaries")]
    gen.run()
    s = open(os.path.join(out, "script")).read()
    return s.replace("\nsync\n", "\n{POKES}\nsync\n"), os.path.join(out, "data")


def program(lines):
    """assemble, tracking format control (setfc) the way the hardware does; it starts
    with the colour C10 select on (hwt_p6)"""
    hw.CTX["fc"] = 2
    words = []
    for t in lines:
        if t.startswith("raw "):            # "raw <hi> <lo>": a word the assembler (rightly) refuses
            hi, lo = (int(x, 16) for x in t.split()[1:3])
        else:
            hi, lo = asm.assemble(t)
        hw.track(hw.decode(lo, hi))
        words += [lo, hi]
    hw.CTX["fc"] = 2
    return words


def script(tmpl, name, words, pokes=(), full=False):
    # no words: leave the captured main program (`mov o0, sa9`, the uniform colour) in place
    p = [f"poke {PROG:#x} " + " ".join(f"{w:#010x}" for w in words)] if words else []
    p += [f"poke {va:#x} " + " ".join(f"{w:#010x}" for w in ws) for va, ws in pokes]
    y, x, n = ROW
    s = tmpl.replace("{POKES}", "\n".join(p))
    s += f"dump {FB + (y * PITCH + x) * 4:#x} {n * 4:#x} {BOX}/{name}.row\ndump {FB:#x} 0x4 {BOX}/{name}.bg\n"
    if full:
        s += f"dump {FB:#x} {PITCH * 480 * 4:#x} {BOX}/{name}.fb\n"
    return s


RUNNER = r"""
cd %(box)s
# the replay needs the GPU to itself (its buffers must land at the captured addresses):
# wait while another process has the PVR device open, and retry a draw that collided (rc 2)
busy() { for p in /proc/[0-9]*; do [ "${p#/proc/}" = "$$" ] && continue; ls -l $p/fd 2>/dev/null | grep -q /dev/dri/card0 && return 0; done; return 1; }
for t in $(cat list); do
  for try in 1 2 3 4 5; do
    i=0; while busy && [ $i -lt 300 ]; do sleep 1; i=$((i+1)); done
    rm -f $t.row $t.bg
    n=$(dmesg | wc -l)
    ./replay $t.s data > $t.log 2>&1 & P=$!
    ( sleep 20; kill -9 $P 2>/dev/null && echo WATCHDOG ) & W=$!
    wait $P; rc=$?; kill $W 2>/dev/null
    [ $rc = 2 ] && grep -q "landed at" $t.log && { echo "$t: another GPU client, retry"; sleep 5; continue; }
    break
  done
  k=$(dmesg | tail -n +$((n+1)) | grep -v -E 'pvr-compat|PVRSRVOpenDCDeviceKM|OSAcquirePhysPageAddr' | grep -i -E 'recover|lockup|hwr|timeout|error' | head -3)
  echo "$t rc=$rc $k"
  if [ $rc != 0 ] || [ -n "$k" ]; then echo "STOP at $t"; tail -5 $t.log; break; fi
done
tar cf res.tar *.row *.bg $(ls *.fb 2>/dev/null) 2>/dev/null
"""


def run_box(work, tests):
    ea3 = env("EA3SH")
    sh = lambda cmd, inp=None: subprocess.run([ea3, cmd], input=inp, capture_output=True, check=False)
    sh(f"rm -rf {BOX}; mkdir -p {BOX}")
    files = {"replay": open(env("REPLAY"), "rb").read(), "data": open(os.path.join(work, "base", "data"), "rb").read(),
             "list": "\n".join(n for n, *_ in tests).encode(), "run.sh": (RUNNER % {"box": BOX}).encode()}
    for n, *_ in tests:
        files[f"{n}.s"] = open(os.path.join(work, f"{n}.s"), "rb").read()
    # one tar over one ssh
    import io
    import tarfile
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for k, v in files.items():
            ti = tarfile.TarInfo(k)
            ti.size = len(v)
            ti.mode = 0o755
            tf.addfile(ti, io.BytesIO(v))
    sh(f"cd {BOX} && tar xf -", buf.getvalue())
    r = sh(f"sh {BOX}/run.sh")
    log = r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace")
    r = sh(f"cat {BOX}/res.tar")
    res = {}
    with tarfile.open(fileobj=io.BytesIO(r.stdout)) as tf:
        for m in tf.getmembers():
            res[m.name] = tf.extractfile(m).read()
    return log, res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("suite")
    ap.add_argument("-k", default="")
    ap.add_argument("--dry", action="store_true", help="assemble and print, don't run")
    ap.add_argument("--full", metavar="DIR", help="also dump each test's whole back buffer (BGRA8888, "
                    "736-pixel stride, 480 rows) to DIR/hwt_frame_<test>.bgra")
    ap.add_argument("--work", default=os.environ.get("HWT_WORK", "/tmp/hwt"))
    a = ap.parse_args()
    sys.path.insert(0, os.getcwd())
    suite = importlib.import_module(a.suite)
    tests = [t for t in suite.TESTS if a.k in t[0]]
    os.makedirs(a.work, exist_ok=True)
    for name, lines, *_ in tests:
        print(f"-- {name}")
        for t, (lo, hi) in zip(lines, zip(*[iter(program(lines))] * 2)):
            print(f"   {hi:08x} {lo:08x}  {t}")
    if a.dry:
        return
    tmpl, _ = base(a.work)
    for t in tests:
        name, lines = t[0], t[1]
        pokes = t[3] if len(t) > 3 else ()
        open(os.path.join(a.work, f"{name}.s"), "w").write(script(tmpl, name, program(lines) if lines else [], pokes,
                                                                  full=bool(a.full)))
    log, res = run_box(a.work, tests)
    print(log.strip())
    if a.full:
        os.makedirs(a.full, exist_ok=True)
        for name, *_ in tests:
            if f"{name}.fb" in res:
                open(os.path.join(a.full, f"hwt_frame_{name}.bgra"), "wb").write(res[f"{name}.fb"])
    bad = 0
    for name, lines, expect, *_ in tests:
        row, bg = res.get(f"{name}.row"), res.get(f"{name}.bg")
        if row is None:
            print(f"{name:24s} NO RESULT")
            bad += 1
            continue
        ws = struct.unpack(f"<{len(row) // 4}I", row)
        w = ws[0]
        uni = "" if len(set(ws)) == 1 else f" NONUNIFORM {sorted(set(ws))}"
        bgw = struct.unpack("<I", bg)[0]
        ok = "" if expect is None else ("ok" if (expect(w) if callable(expect) else w == expect) else
                                         ("FAIL" + ("" if callable(expect) else f" want {expect:#010x}")))
        if ok.startswith("FAIL"):
            bad += 1
        f = struct.unpack("<f", struct.pack("<I", w))[0]
        h = struct.unpack("<2e", struct.pack("<I", w))
        print(f"{name:24s} {w:#010x}  f32 {f:<14.8g} f16 ({h[0]:g}, {h[1]:g})  {ok}{uni}"
              + ("" if bgw == 0xcc336699 else f" bg={bgw:#010x}"))
    shutil.rmtree(os.path.join(a.work, "base"), ignore_errors=True)
    return bad


if __name__ == "__main__":
    sys.exit(main())
