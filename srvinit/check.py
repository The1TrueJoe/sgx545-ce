#!/usr/bin/env python3
"""Check srvinit against a pvrtrace of the DDK's SrvInit (tools/trace, sgxinit).

    check.py plan <trace.bin> > plan.txt        addresses the blob got, for srvinit -n
    check.py diff <trace.bin> <srvinit -n outdir>

diff compares every buffer srvinit writes with the bytes the blob had written
when libsrv_um unmapped it, and every SGX_DEVINITPART2 field (the trace is
i386, srvinit's dump is the native x86_64 struct; handles compare by which
allocation they name).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "decode"))
import pvrlog  # noqa: E402

NAMES = ["ccb", "ccbctl", "kicker", "ta3dctl", "hostctl", "misc", "hwperf", "tasig", "3dsig",
         "ukernel", "pds0", "pds1", "pds2", "pds3", "pds4"]

# SGX_BRIDGE_INIT_INFO for this build: (name, kind, count); kind h = handle, u = u32
FIELDS = [("hKernelCCBMemInfo", "h", 1), ("hKernelCCBCtlMemInfo", "h", 1),
          ("hKernelCCBEventKickerMemInfo", "h", 1), ("hKernelSGXHostCtlMemInfo", "h", 1),
          ("hKernelSGXTA3DCtlMemInfo", "h", 1), ("hKernelSGXMiscMemInfo", "h", 1),
          ("aui32HostKickAddr", "u", 11), ("sScripts", "u", 144 * 3),
          ("ui32ClientBuildOptions", "u", 1), ("sSGXStructSizes", "u", 13),
          ("hKernelHWPerfCBMemInfo", "h", 1), ("hKernelTASigBufferMemInfo", "h", 1),
          ("hKernel3DSigBufferMemInfo", "h", 1),
          ("ui32EDMTaskReg0", "u", 1), ("ui32EDMTaskReg1", "u", 1), ("ui32ClkGateCtl", "u", 1),
          ("ui32ClkGateCtl2", "u", 1), ("ui32ClkGateStatusReg", "u", 1),
          ("ui32ClkGateStatusMask", "u", 1), ("ui32CacheControl", "u", 1),
          ("asInitDevData", "u", 24), ("asInitMemHandles", "h", 18)]


def parse(b, hsize):
    """Field name -> list of values, walking the struct with hsize-byte handles."""
    o, r = 0, {}
    for name, kind, n in FIELDS:
        if kind == "h":
            o = (o + hsize - 1) // hsize * hsize
            fmt, sz = ("<Q", 8) if hsize == 8 else ("<I", 4)
        else:
            fmt, sz = "<I", 4
        r[name] = [struct.unpack_from(fmt, b, o + i * sz)[0] for i in range(n)]
        o += n * sz
    return r


def trace(path):
    log = pvrlog.load(path)
    allocs, cookie, recs = [], {}, log.recs
    for r in recs:
        if isinstance(r, pvrlog.Bridge) and r.name == "ALLOC_DEVICEMEM":
            heap = struct.unpack_from("<I", r.inp, 8)[0]
            dev, size = struct.unpack_from("<I", r.out, 16)[0], struct.unpack_from("<I", r.out, 32)[0]
            allocs.append({"dev": dev, "size": size, "heap": heap, "map": None})
        elif isinstance(r, pvrlog.Bridge) and r.name == "MHANDLE_TO_MMAP_DATA":
            cookie[struct.unpack_from("<I", r.out, 4)[0]] = len(allocs) - 1
            allocs[-1]["byteoff"] = struct.unpack_from("<I", r.out, 8)[0]
        elif isinstance(r, pvrlog.Mmap):
            allocs[cookie[r.off >> 12]]["map"] = r.addr
    snaps = {s.addr: s.data for s in log.of(pvrlog.Snap) if s.reason == "unmap"}
    for a in allocs:
        a["data"] = snaps[a["map"]][a["byteoff"]:a["byteoff"] + a["size"]]
    br = {r.name: r for r in log.of(pvrlog.Bridge)}
    return allocs, br


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    allocs, br = trace(path)
    if cmd == "plan":
        out = br["SGXINFO_FOR_SRVINIT"].out
        print("pd 0x%x" % struct.unpack_from("<I", out, 4)[0])
        for i in range(32):   # PVRSRV_HEAP_INFO i386: id, handle, base, ...: 24 bytes
            hid, _, base = struct.unpack_from("<III", out, 8 + i * 24)
            if hid != 0xffffffff:
                print("heap %d 0x%x" % (hid & 0xffffff, base))
        for i, a in enumerate(allocs):
            print("buf %d 0x%x" % (i, a["dev"]))
        return 0

    outdir, bad = sys.argv[3], 0
    for i, a in enumerate(allocs):
        ours = open(os.path.join(outdir, NAMES[i] + ".bin"), "rb").read()
        diff = [j for j in range(0, a["size"], 4) if ours[j:j + 4] != a["data"][j:j + 4]]
        print("%-8s %08x %5x bytes: %s" % (NAMES[i], a["dev"], a["size"],
                                          "identical" if not diff else "%d words differ" % len(diff)))
        for j in diff[:8]:
            print("    +%04x blob %08x ours %08x" % (j, struct.unpack_from("<I", a["data"], j)[0],
                                                     struct.unpack_from("<I", ours, j)[0]))
        bad += bool(diff)
    blob = parse(br["SGX_DEVINITPART2"].inp[8:], 4)
    ours = parse(open(os.path.join(outdir, "initinfo.bin"), "rb").read(), 8)
    first = min(v for k, kind, _ in FIELDS if kind == "h" for v in blob[k] if v)
    for name, kind, _ in FIELDS:
        b, o = blob[name], ours[name]
        if kind == "h":   # which allocation each handle names; 0 stays 0
            b = [v - first if v else None for v in b]
            o = [v - 1 if v else None for v in o]
        ok = b == o
        bad += not ok
        print("%-30s %s" % (name, "same" if ok else "DIFFERENT"))
        if not ok:
            print("    blob", b[:24], "\n    ours", o[:24])
    print("ALL MATCH" if not bad else "%d mismatches" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
