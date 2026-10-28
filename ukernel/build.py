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
    for kind, val in a.lines:
        if kind == "label":
            pending.append(val)
        else:
            for name in pending:
                offset[name] = idx
            pending.clear()
            idx += 1
    for name in pending:          # labels at the very end point past the code
        offset[name] = idx
    # second pass: encode, substituting `ba LABEL` / `br LABEL` targets
    words = []
    idx = 0
    for kind, val in a.lines:
        if kind == "label":
            continue
        text = val
        for name, off in offset.items():
            if f" {name}" in text or text.endswith(name):
                # a branch to a named label: ba is absolute, br is relative
                if text.split()[0].lstrip("(").split(")")[-1].strip() in ("ba",):
                    text = text.replace(name, str(off))
                elif text.split()[0] == "br":
                    text = text.replace(name, str(off - idx))
        hi, lo = asm.assemble(text)
        words.append((lo, hi))
        idx += 1
    return words, offset


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "sgx545-ukernel-openhc.bin")
    man = sys.argv[2] if len(sys.argv) > 2 else out.rsplit(".", 1)[0] + ".json"
    words, offset = assemble()
    data = b"".join(struct.pack("<II", lo, hi) for lo, hi in words)
    with open(out, "wb") as f:
        f.write(data)
    manifest = {
        "bytes": len(data),
        "instructions": len(words),
        "entries": {k: v * 8 for k, v in offset.items()},   # byte offsets
    }
    with open(man, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"{out}: {len(data)} bytes, {len(words)} instructions")
    print(f"{man}: entries " + ", ".join(f"{k}=0x{v * 8:x}" for k, v in offset.items()))


if __name__ == "__main__":
    main()
