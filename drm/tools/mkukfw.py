#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Wrap an open microkernel image in the file powervr_sgx loads (docs/UKERNEL.md 4a).

    mkukfw.py IMAGE -o OUT [--entry PROG:OFF]... [--patch OFF:VALUE]...
              [--features N] [--interface MAJOR.MINOR] [--core-rev N] [--build-id TEXT]
    mkukfw.py IMAGE -o OUT --manifest JSON    the same, from ukernel/build.py's manifest:
              entries.event, patches.ctlblk and, if present, "features" and "build_id"
    mkukfw.py --check FILE      print and check a file as the driver does

VALUE is a number or one of ta3dctl, hostctl, ccbctl, ccb, misc, pdtable.
"""
import argparse
import struct
import sys
import zlib

MAGIC = (0x35584753, 0x4E524B55)
HEADER = struct.Struct("<2I10I4I32s")
VALUES = {"ta3dctl": 1, "hostctl": 2, "ccbctl": 3, "ccb": 4, "misc": 5, "pdtable": 6}
FEATURES = 0xf  # CLEANUP_V2 | SECURE | DONE_SEQ | SINGLE_CONTEXT, the driver's UKFW_FEATURES
SINGLE_CONTEXT = 0x8  # feature bit 3; such an image must carry a pdtable patch
PRIVATE = 0xffff0000  # the microkernel's own build variants, as the driver's UKFW_FEATURES_PRIVATE
MAX_IMAGE = 0xE000 - 0x200 - 0x10


def num(s):
    return int(s, 0)


def pair(s, names=None):
    a, b = s.split(":")
    return num(a), (names[b] if names and b in names else num(b))


def build(image, entries, patches, features, interface, core_rev, build_id):
    hsize = HEADER.size
    eoff = hsize
    poff = eoff + 8 * len(entries)
    body = b"".join(struct.pack("<2I", *e) for e in entries)
    body += b"".join(struct.pack("<2I", *p) for p in patches)
    total = hsize + len(body)
    header_size = (total + 7) & ~7
    hdr = HEADER.pack(*MAGIC, header_size, interface, features, len(image),
                      zlib.crc32(image), len(entries), eoff, len(patches),
                      poff if patches else 0, core_rev, 0, 0, 0, 0,
                      build_id.encode()[:32])
    return hdr + body + b"\0" * (header_size - total) + image


def check(data):
    """The driver's checks (pvrsgx_fw.c, ukfw_load); returns a list of errors."""
    errs = []
    if len(data) < 0x60:
        return ["shorter than the header"]
    f = HEADER.unpack_from(data)
    if f[0:2] != MAGIC:
        return ["no magic"]
    hsize, iface, feat, isize, crc, ne, eoff, np_, poff, rev = f[2:12]
    print(f"header {hsize:#x}, interface {iface >> 16}.{iface & 0xffff}, features {feat:#x}, "
          f"image {isize:#x} crc {crc:#010x}, core rev {rev:#x}, build {f[16].rstrip(bytes(1)).decode()!r}")
    if hsize < 0x60 or hsize % 8 or hsize > len(data) or not isize or isize % 8 \
            or isize > len(data) - hsize or isize > MAX_IMAGE:
        errs.append("bad sizes")
        return errs
    if iface >> 16 != 1 or feat & ~(FEATURES | PRIVATE):
        errs.append("interface or features not supported")
    if zlib.crc32(data[hsize:hsize + isize]) != crc:
        errs.append("image CRC mismatch")

    def table(off, n):
        if not n or n > 64 or off % 4 or off + 8 * n > len(data):
            return None
        return [struct.unpack_from("<2I", data, off + 8 * i) for i in range(n)]

    ents = table(eoff, ne)
    pats = table(poff, np_) if np_ else []
    if ents is None or pats is None:
        errs.append("bad entry or patch table")
        return errs
    for p, o in ents:
        print(f"  entry: program {p} at {o:#x}")
        if p > 4 or o >= isize or o % 8:
            errs.append(f"bad entry {p}:{o:#x}")
    if not any(p == 0 for p, _ in ents):
        errs.append("no program 0 (event) entry")
    names = {v: k for k, v in VALUES.items()}
    for o, v in pats:
        print(f"  patch: {o:#x} <- {names.get(v, v)}")
        if o > isize - 8 or o % 8 or v not in names:
            errs.append(f"bad patch {o:#x}:{v}")
            continue
        w0, w1 = struct.unpack_from("<2I", data, hsize + o)
        if w0 & 0x1FFFFF != 0xDBEEF or w1 & 0x0003F1F0:
            errs.append(f"no limm placeholder at {o:#x}")
    return errs


def selftest():
    """Regression guard: the manifest auto-path must emit EVERY patch. Dropping
    the single-context core's pdtable leaves the microkernel with no PD-table
    base (a broken image). Pack a 530-style manifest and verify both the control
    block (ta3dctl) and the PD table (pdtable) come through."""
    img = bytearray(0x20)
    for off in (8, 16):
        struct.pack_into("<2I", img, off, 0xDBEEF, 0)	# a limm placeholder
    patches = {"ctlblk": 8, "pdtable": 16}
    bufs = {"ctlblk": "ta3dctl", "pdtable": "pdtable"}
    data = build(bytes(img), [pair("0:0")],
                 [pair(f"{o}:{bufs[n]}", VALUES) for n, o in patches.items()],
                 SINGLE_CONTEXT, 1 << 16, 0, "")
    f = HEADER.unpack_from(data)	# ...hsize,iface,feat,isize,crc,ne,eoff,np,poff,rev
    vals = {struct.unpack_from("<2I", data, f[10] + 8 * i)[1] for i in range(f[9])}
    ok = not check(data) and {VALUES["ta3dctl"], VALUES["pdtable"]} <= vals
    print("mkukfw selftest:",
          "OK" if ok else f"FAILED (patch values {sorted(vals)})")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--entry", action="append", default=[])
    ap.add_argument("--patch", action="append", default=[])
    ap.add_argument("--features", type=num, default=0)
    ap.add_argument("--interface", default="1.0")
    ap.add_argument("--core-rev", type=num, default=0)
    ap.add_argument("--build-id", default="")
    ap.add_argument("--manifest")
    ap.add_argument("--selftest", action="store_true",
                    help="pack a single-context manifest and verify both patches, then exit")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.file:
        ap.error("file is required")
    if a.check:
        errs = check(open(a.file, "rb").read())
        for e in errs:
            print("error:", e)
        return 1 if errs else 0
    if not a.out:
        ap.error("-o is required")
    if a.manifest:
        import json
        m = json.load(open(a.manifest))
        a.entry = a.entry or [f"0:{m['entries']['event']}"]
        # Every manifest patch, not just ctlblk: a single-context core also
        # patches its PD table (pdtable). Map each manifest name to its value.
        bufs = {"ctlblk": "ta3dctl", "pdtable": "pdtable"}
        a.patch = a.patch or [f"{o}:{bufs[n]}" for n, o in m["patches"].items()]
        a.features = m.get("features", a.features)
        a.build_id = a.build_id or m.get("build_id", "")
    major, minor = (int(x) for x in a.interface.split("."))
    patches = [pair(p, VALUES) for p in a.patch]
    if a.features & SINGLE_CONTEXT and VALUES["pdtable"] not in [v for _, v in patches]:
        ap.error("a single-context image (features bit 3) needs a pdtable patch; none given")
    data = build(open(a.file, "rb").read(), [pair(e) for e in a.entry or ["0:0"]],
                 patches, a.features, major << 16 | minor, a.core_rev, a.build_id)
    errs = check(data)
    for e in errs:
        print("error:", e)
    if errs:
        return 1
    open(a.out, "wb").write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
