# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Assemble the open microkernel (ukernel.py) into the binary srvinit loads
and a manifest of its entry offsets.

    python3 ukernel/build.py [core] [out.bin] [manifest.json]

`core` is an optional leading bare token (e.g. `sgx545`) selecting
`cores/<core>.py`; it defaults to sgx545, and because output paths carry a
`.`/`/`, existing callers that pass only out.bin/manifest.json are unchanged.
`SGX_CORE` does the same and, if set, wins.

Lines are assembled by the `sgx-asm` host tool built from mesa/src/sgx/isa's
`sgx.xml`: point `SGX_ASM` at it (`ukernel/sgx-asm.sh` builds one). Labels
resolve to byte offsets of branch/entry targets, and the result is written little-endian, 8 bytes per
instruction. The manifest gives srvinit the entry offsets it must point its
PDS programs and host-kick table at — the open equivalent of the DDK build's
fixed constants (docs/SRVINIT.md, "What still comes from Imagination").
"""
import json
import re
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _select_core():
    """Consume an optional leading bare core token into SGX_CORE, before
    ukernel imports its config. A token with no `/` or `.` names a core; output
    paths always carry one, so existing callers are untouched. SGX_CORE wins."""
    if os.environ.get("SGX_CORE") is not None:
        return
    if len(sys.argv) > 1 and "/" not in sys.argv[1] and "." not in sys.argv[1]:
        tok = sys.argv[1]
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cores", tok + ".py")
        if not os.path.exists(path):
            sys.exit(f"build.py: unknown core {tok!r} (no cores/{tok}.py)")
        os.environ["SGX_CORE"] = tok
        del sys.argv[1]


_select_core()

import ukernel  # noqa: E402


def resolve():
    """Resolve the program to an ordered list of encodable items plus the
    manifest's entry/patch offsets. Each item is ("text", <instruction text>) with
    every immediate numeric (branch targets and per-core symbols resolved here), or
    ("raw", (lo, hi)) for a pre-encoded word. The assembler never sees a symbol."""
    a = ukernel.program()
    resolve.waits = a.waits       # the bounded waits' ids (WAIT_BOUND), for the manifest
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
    # second pass: resolve `ba LABEL` / `br LABEL` to instruction targets (ba is
    # absolute, br is relative to this instruction) and emit the ordered items
    import re
    items = []
    idx = 0
    for kind, val in a.lines:
        if kind == "label":
            continue
        if kind == "raw":                      # a pre-encoded (hi, lo) word
            items.append(("raw", (val[1], val[0])))
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
        items.append(("text", text))
        idx += 1
    return items, offset, patches


def _encode_sgxasm(items, tool):
    """Assemble the text items through the `sgx-asm` host tool (built from
    mesa/src/sgx/isa's sgx.xml). sgx-asm takes fully-resolved text, one
    instruction per line, and `-b` emits raw little-endian lo,hi per instruction;
    raw items are spliced back at their positions (docs/UKERNEL-TOOLING.md §2)."""
    import subprocess
    text_lines = [t for kind, t in items if kind == "text"]
    proc = subprocess.run([tool, "-b", "-c", f"sgx{ukernel.ISA_CORE}"],
                          input=("\n".join(text_lines) + "\n").encode(),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.exit(f"sgx-asm ({tool}) failed:\n{proc.stderr.decode(errors='replace')}")
    blob = proc.stdout
    if len(blob) != 8 * len(text_lines):
        sys.exit(f"sgx-asm emitted {len(blob)} bytes for {len(text_lines)} instructions")
    enc = [struct.unpack_from("<II", blob, 8 * i) for i in range(len(text_lines))]  # (lo, hi)
    words, ti = [], 0
    for kind, v in items:
        if kind == "raw":
            words.append(v)
        else:
            words.append(enc[ti])
            ti += 1
    return words


def temps(items):
    """the temporaries the image needs: its highest rN, plus 1 (i0-i3 are r124+)"""
    n = [int(m.group(1)) for kind, v in items if kind == "text"
         for m in re.finditer(r"\br(\d+)\b", v)]
    return max((r for r in n if r < 124), default=-1) + 1


def assemble():
    items, offset, patches = resolve()
    assemble.items = items
    tool = os.environ.get("SGX_ASM")
    if not tool:
        sys.exit("build.py: set SGX_ASM to an sgx-asm (sh ukernel/sgx-asm.sh DIR builds DIR/sgx-asm)")
    return _encode_sgxasm(items, tool), offset, patches, "sgx-asm"


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), f"{ukernel.CORE_NAME}_ukernel.bin")
    man = sys.argv[2] if len(sys.argv) > 2 else out.rsplit(".", 1)[0] + ".json"
    words, offset, patches, used = assemble()
    data = b"".join(struct.pack("<II", lo, hi) for lo, hi in words)
    with open(out, "wb") as f:
        f.write(data)
    # features: the image's declared capability bits (PVRSGX_UKFW_*). Bit 1 = SECURE
    # (the bgblock is CPU-written, so powervr_sgx drops kernel-heap sharing). Default 2:
    # secure mode is HW-proven and the open default (m2 2026-11-08). NOT bit 0 (cleanup-v2):
    # ours is defer-and-hold. build_id names the build. mkukfw --manifest reads both.
    features = int(os.environ.get("UK_FEATURES", str(ukernel.FEATURES)), 0)
    # Feature bits 0-15 are host-visible and must be known to the driver; bits 16-31 are the
    # microkernel's own build variants, which the driver accepts and ignores (m2-drm 25f9d54).
    if ukernel.XFER_BANK_ALL:
        features |= 1 << 16   # bit 16 = all-data-master transfer bank routing (Cedarview 1.0.13 B)
    build_id = os.environ.get("UK_BUILD_ID", "dev")
    version = "%d.%d" % (ukernel.UK_VERSION >> 16, ukernel.UK_VERSION & 0xffff)
    manifest = {
        "core": ukernel.CORE_NAME,          # the SGX core this image targets (per-core)
        "version": version,                 # microkernel interface version, major.minor
        "bytes": len(data),
        "instructions": len(words),
        "features": features,
        "build_id": build_id,
        "assembler": used,                  # "sgx-asm" (from sgx.xml)
        "entries": {k: v * 8 for k, v in offset.items()},    # byte offsets
        "patches": {k: v * 8 for k, v in patches.items()},   # limm sites the loader fills
    }
    manifest["temps"] = temps(assemble.items)    # r registers the image uses (highest + 1): what an
                                        # entry DOUTU's temporary count has to cover
    if f"sgx{ukernel.ISA_CORE}" != ukernel.CORE_NAME:   # assembled with another core's encodings
        manifest["isa"] = f"sgx{ukernel.ISA_CORE}"
    if resolve.waits:             # a core built with bounded waits: id -> what it polls
        manifest["waits"] = {str(i): {"poll": "0x%x" % p, "mask": "0x%x" % m, "until": u}
                             for i, p, m, u in resolve.waits}
    with open(man, "w") as f:
        json.dump(manifest, f, indent=2)
    # a flat sidecar srvinit -o reads (plain text, fscanf-friendly): the entry
    # the event program starts, and the limm patch sites the loader fills
    with open(out + ".pat", "w") as f:
        f.write(f"entry event {offset['event'] * 8}\n")   # the event program starts this
        for name, i in patches.items():
            f.write(f"patch {name} {i * 8}\n")
    print(f"{out}: {len(data)} bytes, {len(words)} instructions, assembled with {used}")
    print(f"{man}: entries " + ", ".join(f"{k}=0x{v * 8:x}" for k, v in offset.items()))
    print(f"{man}: patches " + ", ".join(f"{k}=0x{v * 8:x}" for k, v in patches.items()))


if __name__ == "__main__":
    main()
