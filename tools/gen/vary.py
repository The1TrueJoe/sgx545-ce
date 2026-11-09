"""Draw test frames with tools/gen/frame on the EA3 and check them against the DDK.

    EA3SH=<ssh helper> python3 tools/gen/vary.py OUTDIR [case...]

For each case the box runs the DDK's probe (tools/probe/probe.c, i386 blob) for
every triangle on its own, and tools/gen/frame for the whole case. The
prediction is the probes' glReadPixels images composed in the case's order
(the first image whose pixel is not the clear colour wins), so two draws and
depth tests are checked against the blob's own rasterisation of each
triangle. A single-probe case compares with the probe directly. Writes
OUTDIR/<case>.bin (frame's render target), <case>-<n>.rgba (the probes),
<case>.png and results.json; exit status 1 if any case differs.

EA3SH is a command that runs its arguments on the EA3 as root (a shell
script around ssh); /tmp/gen/frame must be there already. The probe is
/opt/sgx-i386/bin/probe-st run through /opt/sgx-i386/run.sh.
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "decode"))
from cmpfb import convert, png  # noqa: E402

A = "-0.7,-0.6,0.5,-0.4,-0.1,0.7"       # two overlapping triangles
B = "-0.4,0.6,-0.5,-0.7,0.7,0.1"
RED, BLUE, ORANGE = "1,0,0,1", "0,0,1,1", "1,0.5,0,1"
REF = "-0.5,-0.5,0.5,-0.5,0,0.5"

# name: (frame arguments, [probe environments in priority order], clear r,g,b,a[, (w, h) pbuffer])
CASES = {
    "ref": ("", [{}], "0,0,0.5,1"),
    "colors": ("-k 0.25,0,0,1 -T -0.8,-0.6,0.2,-0.8,-0.3,0.7:" + ORANGE,
               [{"PROBE_CLEAR": "0.25 0 0 1", "PROBE_VERTS": "-0.8 -0.6 0.2 -0.8 -0.3 0.7",
                 "PROBE_COLOR": "1 0.5 0 1"}], "0.25,0,0,1"),
    # two draws, no depth test: the second covers the first
    "two": (f"-T {A}:{RED} -T {B}:{BLUE}",
            [{"PROBE_VERTS": B, "PROBE_COLOR": "0 0 1 1"}, {"PROBE_VERTS": A, "PROBE_COLOR": "1 0 0 1"}],
            "0,0,0.5,1"),
    # depth test LESS, the near (red, z -0.5) triangle drawn first: the far one loses where they overlap
    "depth": (f"-d 0x201 -T {A},-0.5:{RED} -T {B},0.5:{BLUE}",
              [{"PROBE_VERTS": A, "PROBE_COLOR": "1 0 0 1"}, {"PROBE_VERTS": B, "PROBE_COLOR": "0 0 1 1"}],
              "0,0,0.5,1"),
    # the same pair, far first: the near one still wins (and covers what it overlaps)
    "depth-far-first": (f"-d 0x201 -T {B},0.5:{BLUE} -T {A},-0.5:{RED}",
                        [{"PROBE_VERTS": A, "PROBE_COLOR": "1 0 0 1"},
                         {"PROBE_VERTS": B, "PROBE_COLOR": "0 0 1 1"}], "0,0,0.5,1"),
    # GREATER against the background depth 1.0: nothing passes, the frame is the clear alone
    "depth-greater": (f"-d 0x204 -T {A},-0.5:{RED} -T {B},0.5:{BLUE}", [], "0,0,0.5,1"),
    # LESS with depth writes off: the far triangle passes against 1.0 and covers the near one
    "depth-nowrite": (f"-d 0x201,0 -T {A},-0.5:{RED} -T {B},0.5:{BLUE}",
                      [{"PROBE_VERTS": B, "PROBE_COLOR": "0 0 1 1"}, {"PROBE_VERTS": A, "PROBE_COLOR": "1 0 0 1"}],
                      "0,0,0.5,1"),
    # glViewport(100, 50, 400, 300)
    "viewport": (f"-V 100,50,400,300 -T {REF}:0,1,0,1", [{"PROBE_VIEWPORT": "100 50 400 300"}], "0,0,0.5,1"),
    # other surface sizes: y-up surfaces against the DDK's pbuffers (2x2 macrotiles below 0x4b000 pixels)
    "pb128x48": (f"-p -s 128x48 -T {A}:{RED} -T {B}:{BLUE}",
                 [{"PROBE_VERTS": B, "PROBE_COLOR": "0 0 1 1"}, {"PROBE_VERTS": A, "PROBE_COLOR": "1 0 0 1"}],
                 "0,0,0.5,1", (128, 48)),
    "pb320x200": (f"-p -s 320x200 -d 0x201 -T {A},-0.5:{RED} -T {B},0.5:{BLUE}",
                  [{"PROBE_VERTS": A, "PROBE_COLOR": "1 0 0 1"}, {"PROBE_VERTS": B, "PROBE_COLOR": "0 0 1 1"}],
                  "0,0,0.5,1", (320, 200)),
}


def sh(cmd, out=None):
    ea3 = os.environ["EA3SH"]
    r = subprocess.run([ea3, cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if out is not None:
        with open(out, "wb") as f:
            f.write(r.stdout)
    return r.stdout


def clear_rgba(s):
    return bytes(max(0, min(255, round(float(x) * 255))) for x in s.split(","))


def run(case, outdir):
    fargs, probes, clear = CASES[case][:3]
    pb = CASES[case][3] if len(CASES[case]) > 3 else None
    w, h = pb or (720, 480)
    imgs = []
    for n, env in enumerate(probes):
        e = " ".join(f"{k}='{v.replace(',', ' ')}'" for k, v in env.items())
        if case not in ("ref",) and "PROBE_CLEAR" not in env:
            e += " PROBE_CLEAR='" + clear.replace(",", " ") + "'"
        if pb:
            e += f" PROBE_PBUFFER='{w} {h}' PROBE_CONFIG='8 8 8 8 0 0'"
        log = sh(f"cd /tmp/gen && env {e} PROBE_OUT=/tmp/gen/p.rgba /opt/sgx-i386/run.sh probe-st 2>&1 | tail -2")
        p = os.path.join(outdir, f"{case}-{n}.rgba")
        sh("cat /tmp/gen/p.rgba; rm -f /tmp/gen/p.rgba", p)
        d = open(p, "rb").read()
        if len(d) != w * h * 4:
            sys.exit(f"vary: probe for {case} gave {len(d)} bytes: {log.decode(errors='replace')}")
        imgs.append(d)
    log = sh(f"cd /tmp/gen && ./frame {fargs} -o /tmp/gen/f.bin 2>&1 | tail -3")
    fb = os.path.join(outdir, f"{case}.bin")
    sh("cat /tmp/gen/f.bin; rm -f /tmp/gen/f.bin", fb)
    got = convert(open(fb, "rb").read(), w, h)
    if pb:      # a y-up surface is already bottom row first: undo convert's flip
        got = b"".join(got[(h - 1 - y) * w * 4:(h - y) * w * 4] for y in range(h))
    c = clear_rgba(clear)
    pred = bytearray(c * (w * h))
    for i in range(w * h):
        for im in imgs:
            px = im[4 * i:4 * i + 4]
            if px != c:
                pred[4 * i:4 * i + 4] = px
                break
        else:
            pred[4 * i:4 * i + 4] = c
    diff = sum(got[4 * i:4 * i + 4] != pred[4 * i:4 * i + 4] for i in range(w * h))
    png(os.path.join(outdir, f"{case}.png"), got, w, h)
    covered = sum(pred[4 * i:4 * i + 4] != c for i in range(w * h))
    print(f"{case:18s} {diff:6d} of {w * h} pixels differ ({covered} drawn)  [{log.decode().strip().splitlines()[-1]}]")
    return {"case": case, "frame_args": fargs, "probes": probes, "differ": diff, "drawn": covered,
            "frame_log": log.decode(errors="replace")}


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    cases = sys.argv[2:] or list(CASES)
    res = [run(c, outdir) for c in cases]
    with open(os.path.join(outdir, "results.json"), "w") as f:
        json.dump(res, f, indent=1)
    sys.exit(1 if any(r["differ"] for r in res) else 0)


if __name__ == "__main__":
    main()
