# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Run the isolation tests of docs/UAPI.md section 7.2 on an EA box.

    python3 tools/iso/iso.py <suite> [-k substring] [--dry] [--out FILE]

A test is a spec for tools/iso/iso.c: buffers, a fragment and/or vertex program written in
tools/isa syntax, the registers to read from the host, what to dump. Tests run one process
each, under `flock /tmp/gpu.lock`, with a watchdog. The batch stops at the first test that
times out or makes the kernel log anything about recovery or faults, because the next
result would mean nothing; the wedge itself is the finding.

Environment: EA3 (host, default 10.0.0.113).
"""
import argparse
import io
import importlib
import os
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "gen"))
import mkusse  # noqa: E402   (tools/gen; pulls in tools/isa)

BOX = "/tmp/iso"

FS_HEAD = ["phas.i #0, #0 [wait=7]"]
# phase 0 of vs_xy with test lines in it, then the rest of it (tools/gen/mkusse.py)
VS_HEAD = ["phas.i @p1, #0"] + mkusse.PROLOGUE
VS_TAIL = ["limm pa3, #0x3f800000 [skipinv]", "limm pa2, #0 [skipinv end]", "p1:", "phas.i @p2, #0 [wait=2]",
           "mov o0, pa0 [skipinv rcnt rpt=4]", "emit.t3 r0, #0, #0 [end f12 f10]", "p2:"] + mkusse.VS_EPILOGUE
END = "[skipinv end mask=1]"


def out(reg):
    return f"mov o0, {reg}  {END}"


def words(lines):
    w, reloc = mkusse.assemble(lines)
    return " ".join(f"{x:08x}" for x in w), reloc


class T:
    """One test. fs/vs are the lines between the program's head and its end (fs must end
    with an instruction carrying `end`; vs falls through into vs_xy's position code)."""

    def __init__(self, name, fs=None, vs=None, fstemps=16, vstemps=8, bufs=(), fills=(), fsa=(), vsa=(),
                 regs=(), dumps=(), fsbuf=None, fsw0=None):
        self.name, self.fs, self.vs = name, fs, vs
        self.fstemps, self.vstemps = fstemps, vstemps
        self.bufs, self.fills, self.fsa, self.vsa = bufs, fills, fsa, vsa
        self.regs, self.dumps, self.fsbuf, self.fsw0 = regs, dumps, fsbuf, fsw0

    def spec(self):
        o = [f"buf {n} {h:x} {a:x} {s:x} {al:x}" for n, h, a, s, al in self.bufs]
        o += [f"fill {n} {off:x} " + " ".join(f"{w:x}" for w in ws) for n, off, ws in self.fills]
        o += [f"fsa {i:x} {r}" for i, r in self.fsa]
        o += [f"vsa {i:x} {r}" for i, r in self.vsa]
        if self.fs:
            w, _ = words(FS_HEAD + self.fs)
            o.append(f"fs {w}")
            o.append(f"fstemps {self.fstemps:x}")
            if self.fsbuf:
                o.append(f"fsbuf {self.fsbuf[0]} {self.fsbuf[1]:x}")
            if self.fsw0:
                o.append("fsw0 {:x} {:x} {:x}".format(*self.fsw0))
        if self.vs is not None:
            w, reloc = words(VS_HEAD + self.vs + VS_TAIL)
            o += [f"vs {w}", "vsreloc " + " ".join(f"{i:x}" for i in reloc), f"vstemps {self.vstemps:x}"]
        o += [f"reg {r:x}" for r in self.regs]
        o += [f"dump {n} {off:x} {c:x}" for n, off, c in self.dumps]
        return "\n".join(o) + "\n"


RUNNER = r"""
cd %(box)s
for t in $(cat list); do
  n=$(dmesg | wc -l)
  ./iso $t.spec > $t.out 2>&1 & P=$!
  ( sleep 15; kill -9 $P 2>/dev/null && echo WATCHDOG >> $t.out ) & W=$!
  wait $P; rc=$?; kill $W 2>/dev/null
  dmesg | tail -n +$((n+1)) | grep -v -E 'pvr-compat|PVRSRVOpenDCDeviceKM|OSAcquirePhysPageAddr' > $t.kmsg
  k=$(grep -i -E 'recover|lockup|hwr|fault|timeout|error|assert' $t.kmsg | head -3)
  echo "== $t rc=$rc"
  cat $t.out
  [ -n "$k" ] && echo "kmsg: $k"
  if [ $rc != 0 ] || [ -n "$k" ] || grep -q -E 'WATCHDOG|TIMEOUT' $t.out; then echo "STOP at $t"; break; fi
done
"""


def ssh(cmd, inp=None, timeout=None):
    host = os.environ.get("EA3", "10.0.0.113")
    return subprocess.run(["sshpass", "-p", os.environ["BOX_PASS"], "ssh", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no",
                           "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", f"root@{host}", cmd],
                          input=inp, capture_output=True, timeout=timeout, check=False)


def run(tests):
    buf = io.BytesIO()
    files = {"iso": open(os.path.join(HERE, "iso"), "rb").read(),
             "list": "\n".join(t.name for t in tests).encode(), "run.sh": (RUNNER % {"box": BOX}).encode()}
    for t in tests:
        files[f"{t.name}.spec"] = t.spec().encode()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for k, v in files.items():
            ti = tarfile.TarInfo(k)
            ti.size, ti.mode = len(v), 0o755
            tf.addfile(ti, io.BytesIO(v))
    ssh(f"rm -rf {BOX}; mkdir -p {BOX}; cd {BOX} && tar xf -", buf.getvalue())
    r = ssh(f"flock /tmp/gpu.lock sh {BOX}/run.sh", timeout=20 * len(tests) + 60)
    return r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("suite")
    ap.add_argument("-k", default="")
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args()
    sys.path.insert(0, HERE)
    tests = [t for t in importlib.import_module(a.suite).TESTS if a.k in t.name]
    for t in tests:
        s = t.spec()
        if a.dry:
            print(f"-- {t.name}\n{s}", end="")
    if a.dry:
        print(f"{len(tests)} tests")
        return
    res = run(tests)
    print(res)
    if a.out:
        with open(a.out, "a") as f:
            f.write(f"### {a.suite} -k '{a.k}'\n{res}\n")


if __name__ == "__main__":
    main()
