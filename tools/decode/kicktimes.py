"""List the SGX kicks of a pvrtrace log (PVRTRACE_NOSNAP is enough) with their
times and SGX_CCB_KICK flags, to see how a client paces its frames: how many
kicks a frame takes, which render target each goes to (its HW sync list), and
what other bridge calls (sync waits, transfers, swaps) sit between them.

  kicktimes.py LOG [FIRST [COUNT]]   kicks FIRST.. (1-based), COUNT of them
"""
import collections
import sys

import layout
import pvrlog

KICK = "_SGX_CCB_KICK_"


def main(path, first=1, count=200):
    log = pvrlog.load(path)
    arch = "x86" if log.ptr == 4 else "x86_64"
    base = layout.layout(arch)["PVRSRV_BRIDGE_IN_DOKICK_TAG"].path("sCCBKick")[0]
    g = lambda k, f: layout.get(arch, KICK, k.inp, f, base)
    t0 = None
    n = 0
    last_t = None
    between = collections.Counter()
    targets = {}
    for r in log.of(pvrlog.Bridge):
        if t0 is None:
            t0 = r.t_ns
        if r.name != "SGX_DOKICK":
            between[r.name] += 1
            continue
        n += 1
        if n < first:
            between.clear()
            last_t = r.t_ns
            continue
        if n >= first + count:
            break
        rt = g(r, "hKernelHWSyncListMemInfo")
        tname = targets.setdefault(rt, f"rt{len(targets)}")
        dt = (r.t_ns - last_t) / 1e6 if last_t is not None else 0.0
        last_t = r.t_ns
        flags = "".join([
            "F" if g(r, "bFirstKickOrResume") else "-",
            "L" if g(r, "bLastInScene") else "-",
            "D" if g(r, "bTADependency") else "-",
        ])
        other = " ".join(f"{k}x{v}" for k, v in sorted(between.items()))
        between.clear()
        print(f"kick {n:5d} t {(r.t_ns - t0) / 1e6:10.2f} ms  +{dt:7.2f}  {tname:4s} {flags} "
              f"dst {g(r, 'ui32NumDstSyncObjects')} src {g(r, 'ui32NumSrcSyncs')} "
              f"ta/3d status {g(r, 'ui32NumTAStatusVals')}/{g(r, 'ui32Num3DStatusVals')} "
              f"ccb {g(r, 'ui32CCBOffset'):#06x} ret {r.ret}  | {other}")


if __name__ == "__main__":
    a = sys.argv
    main(a[1], int(a[2]) if len(a) > 2 else 1, int(a[3]) if len(a) > 3 else 200)
