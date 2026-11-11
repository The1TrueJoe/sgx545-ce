"""Run hwt suites on the open stack: sgx_hwt (mesa/src/gallium/winsys/sgx/drm)
assembles each test on the box with Mesa's isa library and draws it through the
winsys, with no captured frame and nothing from the vendor stack.

    python3 -I tools/isa/hwt_open.py <suite module> [-k substring] [--dry]

Same suites as hwt.py (TESTS = [(name, [insn, ...], expect), ...]). A test's
last line moves its result to o0 and ends the program (hwt_lib.out), before the
winsys's colour epilogue. Tests that poke the replay (a fourth field) don't
apply and are skipped. A suite may give BUFS = {name: [dwords]}:
the data buffer's first dwords for that test; BUF in a line is its address.
TEXS = {name: texel} gives that test an 8x8 RGBA8 texture in the texture heap,
every texel the same; TEX in a line is its address.

Environment: BOX_SH (an ssh helper: `$BOX_SH 'cmd'` runs cmd on the box, stdin
passed through), SGX_HWT (the box's path to sgx_hwt; default: the one the
image installs).
"""
import argparse
import importlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = re.compile(r"^mov o0, (\S+)\s+\[.*\bend\b.*\]$")
CLEAR = 0xCC336699  # the clear colour: the program never ran


def convert(lines):
    """the suite's lines, or None when it doesn't end in its move to o0"""
    lines = [ln.strip() for ln in lines]
    return lines if OUT.match(lines[-1]) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("suite")
    ap.add_argument("-k", default="")
    ap.add_argument("--dry", action="store_true", help="print the test file, don't run")
    a = ap.parse_args()
    sys.path.insert(0, HERE)
    sys.path.insert(0, os.getcwd())
    suite = importlib.import_module(a.suite)
    bufs = getattr(suite, "BUFS", {})
    texs = getattr(suite, "TEXS", {})
    tests, text = [], []
    for t in suite.TESTS:
        name, lines, expect = t[0], t[1], t[2]
        if a.k not in name:
            continue
        conv = convert(lines) if len(t) < 4 else None
        if conv is None:
            print(f"{name:24s} skipped (replay pokes or no final out)", file=sys.stderr if a.dry else sys.stdout)
            continue
        tests.append((name, expect))
        text.append(f"test {name}")
        if name in bufs:
            text.append("buf " + " ".join(f"{w:08x}" for w in bufs[name]))
        if name in texs:
            text.append(f"tex {texs[name]:08x}")
        text += conv + ["end"]
    src = "\n".join(text) + "\n"
    if a.dry:
        sys.stdout.write(src)
        return
    box = os.environ.get("BOX_SH")
    if not box:
        sys.exit("hwt_open: set BOX_SH")
    exe = os.environ.get("SGX_HWT", "$(ls /usr/lib/sgx_hwt /opt/sgx/lib/sgx_hwt 2>/dev/null | head -1)")
    r = subprocess.run([box, f"cat > /tmp/hwt_open.txt && {exe} /tmp/hwt_open.txt"], input=src.encode(),
                       capture_output=True, check=False)
    got = {}
    for ln in r.stdout.decode(errors="replace").splitlines():
        p = ln.split()
        if p:
            got[p[0]] = p[1:]
    err = r.stderr.decode(errors="replace").strip()
    if err:
        print(err)
    bad = 0
    for name, expect in tests:
        g = got.get(name)
        if not g:
            print(f"{name:24s} NO RESULT")
            bad += 1
            continue
        if g[0] in ("error", "mixed"):
            print(f"{name:24s} {' '.join(g)}")
            bad += g[0] == "error"
            continue
        w = int(g[0], 16)
        ok = "" if expect is None else ("ok" if (expect(w) if callable(expect) else w == expect) else
                                         "FAIL" + ("" if callable(expect) else f" want {expect:#010x}"))
        bad += ok.startswith("FAIL")
        print(f"{name:24s} {w:#010x}  {ok}{' NEVER RAN' if w == CLEAR else ''} {' '.join(g[1:])}")
    sys.exit(1 if bad or r.returncode else 0)


if __name__ == "__main__":
    main()
