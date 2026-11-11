#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Rebuild a core's serial image and verify it against the recorded golden
hash (ukernel/golden/<core>.sha256, default sgx545). The image is never stored
in the repo, so the hash + size is the byte-for-byte golden. Exit 1 on any drift.

    python3 ukernel/golden/check.py             # build + verify sha256 and size
    python3 ukernel/golden/check.py sgx530      # the same for another core
    python3 ukernel/golden/check.py --ref R.bin # on a mismatch, print the first
                                                # differing 32-bit word vs R.bin,
                                                # a locally built reference image
    python3 ukernel/golden/check.py --write     # rewrite sgx545.sha256 from a
                                                # fresh build (intentional change)
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

GOLDEN = os.path.dirname(os.path.abspath(__file__))        # ukernel/golden
REPO = os.path.dirname(os.path.dirname(GOLDEN))            # repo root
BUILD = os.path.join(REPO, "ukernel", "build.py")
CORE = "sgx545"
SHAFILE = os.path.join(GOLDEN, CORE + ".sha256")


def build_image():
    """Build the default (sgx545, serial) image to a temp file; return bytes."""
    fd, binp = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    jsonp = binp[:-4] + ".json"
    env = dict(os.environ)
    env.pop("UK_OVERLAP", None)                             # the golden is serial
    env.pop("SGX_CORE", None)
    subprocess.run([sys.executable, BUILD, CORE, binp, jsonp], check=True,
                   stdout=subprocess.DEVNULL, env=env)
    with open(binp, "rb") as f:
        data = f.read()
    os.remove(binp)
    if os.path.exists(jsonp):
        os.remove(jsonp)
    if os.path.exists(binp + ".pat"):
        os.remove(binp + ".pat")
    return data


def read_golden():
    sha = size = None
    with open(SHAFILE) as f:
        for line in f:
            p = line.split()
            if len(p) >= 2 and p[0] == "sha256":
                sha = p[1]
            elif len(p) >= 2 and p[0] == "bytes":
                size = int(p[1])
    if sha is None or size is None:
        sys.exit(f"{SHAFILE}: missing sha256/bytes line")
    return sha, size


def first_diff_word(golden_bin, built):
    """Print the first differing 32-bit little-endian word vs a reference."""
    with open(golden_bin, "rb") as f:
        ref = f.read()
    n = min(len(ref), len(built))
    for i in range(0, n, 4):
        rw = struct.unpack_from("<I", ref, i)[0]
        bw = struct.unpack_from("<I", built, i)[0]
        if rw != bw:
            print(f"first differing word: #{i // 8} byte 0x{i:x} -- "
                  f"reference 0x{rw:08x} != built 0x{bw:08x}")
            return
    if len(ref) != len(built):
        print(f"lengths differ: reference {len(ref)} B, built {len(built)} B")
    else:
        print("no differing word (reference matches the built image)")


def main(argv):
    global CORE, SHAFILE
    if argv and not argv[0].startswith("-"):
        CORE = argv.pop(0)
        SHAFILE = os.path.join(GOLDEN, CORE + ".sha256")
    name = CORE.upper()
    built = build_image()
    got_sha = hashlib.sha256(built).hexdigest()
    got_size = len(built)
    if "--write" in argv:
        with open(SHAFILE) as f:
            head = [ln for ln in f if ln.startswith("#")]
        with open(SHAFILE, "w") as f:
            f.writelines(head)
            f.write(f"sha256 {got_sha}\n")
            f.write(f"bytes  {got_size}\n")
        print(f"{SHAFILE}: wrote sha256 {got_sha}, {got_size} B")
        return 0
    sha, size = read_golden()
    if got_size == size and got_sha == sha:
        print(f"{name} serial image matches the golden ({got_sha}, {got_size} B).")
        return 0
    print(f"::error::{name} serial image drifted from ukernel/golden/{CORE}.sha256")
    print(f"  expected sha256 {sha} ({size} B)")
    print(f"  built    sha256 {got_sha} ({got_size} B)")
    if "--ref" in argv:
        ref = argv[argv.index("--ref") + 1]
        first_diff_word(ref, built)
    else:
        print("  for the first differing word, build a reference from a known-good")
        print("  checkout and re-run with --ref <that image>.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
