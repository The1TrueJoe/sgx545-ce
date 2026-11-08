# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Assemble openHC's microkernel (ukernel.py) into the binary srvinit loads
and a manifest of its entry offsets.

    python3 ukernel/build.py [out.bin] [manifest.json]

Each line is assembled by tools/isa/asm.py (libusc's own encoding, so nothing
the hardware can't accept is emitted), labels resolve to byte offsets of
branch/entry targets, and the result is written little-endian, 8 bytes per
instruction. The manifest gives srvinit the entry offsets it must point its
PDS programs and host-kick table at — the open equivalent of the DDK build's
fixed constants (docs/SRVINIT.md, "What still comes from Imagination").
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "isa"))
import asm  # noqa: E402

import ukernel  # noqa: E402


def assemble():
    a = ukernel.program()
    # first pass: assign each real instruction an index; labels mark the next one
    idx = 0
    pending = []
    offset = {}
    patches = {}                  # name -> instruction index (patch sites)
    for kind, val in a.lines:
        if kind == "label":
            pending.append(val)
        else:
            for name in pending:
                offset[name] = idx
            pending.clear()
            if kind == "patch":
                patches[val[1]] = idx
            idx += 1
    for name in pending:          # labels at the very end point past the code
        offset[name] = idx
    # second pass: encode, resolving `ba LABEL` / `br LABEL` to instruction
    # targets (ba is absolute, br is relative to this instruction)
    import re
    words = []
    idx = 0
    for kind, val in a.lines:
        if kind == "label":
            continue
        if kind == "raw":                      # a pre-encoded (hi, lo) word
            words.append((val[1], val[0]))
            idx += 1
            continue
        if kind == "patch":
            text = f"limm r{val[0]}, #0xdbeef"  # 0xdbeef placeholder: the loader fills the
                                                # immediate; powervr_sgx's loader checks the
                                                # sentinel before patching (our srvinit's
                                                # limm_ours does not, so either works)
        else:
            text = val
        m = re.match(r"(\(!?p\d\)\s*)?(ba|br)\s+([A-Za-z_]\w*)\s*(\[.*\])?$", text.strip())
        if m:
            name = m.group(3)
            if name not in offset:
                raise ValueError(f"branch to undefined label {name!r} in {text!r}")
            tgt = offset[name] if m.group(2) == "ba" else offset[name] - idx
            text = text.replace(name, f"#{tgt}")
        hi, lo = asm.assemble(text)
        words.append((lo, hi))
        idx += 1
    return words, offset, patches


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), f"{ukernel.CORE_NAME}-ukernel-openhc.bin")
    man = sys.argv[2] if len(sys.argv) > 2 else out.rsplit(".", 1)[0] + ".json"
    words, offset, patches = assemble()
    data = b"".join(struct.pack("<II", lo, hi) for lo, hi in words)
    with open(out, "wb") as f:
        f.write(data)
    # features: the image's declared capability bits (PVRSGX_UKFW_*). Bit 1 = SECURE
    # (the bgblock is CPU-written, so powervr_sgx drops kernel-heap sharing). Default 2:
    # secure mode is HW-proven and the open default (m2 2026-11-08). NOT bit 0 (cleanup-v2):
    # ours is defer-and-hold. build_id names the build. mkukfw --manifest reads both.
    features = int(os.environ.get("UK_FEATURES", "2"), 0)
    build_id = os.environ.get("UK_BUILD_ID", "openhc")
    version = "%d.%d" % (ukernel.OPENHC_UK_VERSION >> 16, ukernel.OPENHC_UK_VERSION & 0xffff)
    manifest = {
        "core": ukernel.CORE_NAME,          # the SGX core this image targets (per-core)
        "version": version,                 # microkernel interface version, major.minor
        "bytes": len(data),
        "instructions": len(words),
        "features": features,
        "build_id": build_id,
        "entries": {k: v * 8 for k, v in offset.items()},    # byte offsets
        "patches": {k: v * 8 for k, v in patches.items()},   # limm sites the loader fills
    }
    with open(man, "w") as f:
        json.dump(manifest, f, indent=2)
    # a flat sidecar srvinit -o reads (plain text, fscanf-friendly): the entry
    # the event program starts, and the limm patch sites the loader fills
    with open(out + ".pat", "w") as f:
        f.write(f"entry event {offset['event'] * 8}\n")   # the event program starts this
        for name, i in patches.items():
            f.write(f"patch {name} {i * 8}\n")
    print(f"{out}: {len(data)} bytes, {len(words)} instructions")
    print(f"{man}: entries " + ", ".join(f"{k}=0x{v * 8:x}" for k, v in offset.items()))
    print(f"{man}: patches " + ", ".join(f"{k}=0x{v * 8:x}" for k, v in patches.items()))


if __name__ == "__main__":
    main()
