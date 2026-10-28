"""Turn a pvrtrace log into a script for re/replay/replay (and its data file).

    python3 mkreplay.py <log.bin> <outdir> [--skip-heaps 8] [--delta-skip-heaps 1,8,10]

Writes <outdir>/script (text, one op per line) and <outdir>/data (the bytes
the write ops point into). Buffers are named by their index in capture order
(allocations and wraps both count), handles as m<idx> (mem info) or s<idx>
(sync info), so the replay never sees a captured kernel handle.

Memory state before kick 1 is the kick-1 snapshot: each buffer outside the
skipped heaps is zeroed, then its non-zero runs are written. Before each later
kick only the words that changed since the previous kick's snapshot are written,
outside the delta-skipped heaps (the ones the GPU itself writes).
"""
import argparse
import os
import struct
import sys

import layout
import memmap
import pvrlog

ARCH = "x86"
K = "_SGX_CCB_KICK_"


def g(rec, buf, path, base=0):
    return layout.get(ARCH, rec, buf, path, base)


class Gen:
    def __init__(self, log, out, skip, dskip, sleep_ms, wait_ms):
        self.log = log
        self.mm = memmap.build(log)
        self.out = out
        self.skip = skip
        self.dskip = dskip
        self.sleep_ms = sleep_ms
        self.wait_ms = wait_ms
        self.lines = []
        self.data = bytearray()
        self.bufs = []          # memmap.Buf in capture order (allocs and wraps)
        self.handles = {}       # captured handle -> "m<i>" / "s<i>"
        self.heapid = {}        # buf index -> SGX heap id (low byte), or None for wraps
        self.ctx_va = None

    def op(self, *a):
        self.lines.append(" ".join(str(x) for x in a))

    def blob(self, b):
        o = len(self.data)
        self.data += b
        return o

    def hx(self, v):
        return f"{v:#x}"

    def handle(self, h):
        if h == 0:
            return "0"
        if h not in self.handles:
            sys.exit(f"mkreplay: captured handle {h:#x} is not a buffer this replay creates")
        return self.handles[h]

    # -- setup -----------------------------------------------------------------
    def setup(self):
        by_seq = {b.seq: b for b in self.mm.bufs if b.origin in ("alloc", "wrap")}
        acquired = memctx = False
        for r in self.log.of(pvrlog.Bridge):
            if r.ret != 0:
                continue
            if r.name == "CONNECT_SERVICES" and not self.lines:
                self.op("connect")
            elif r.name == "ACQUIRE_DEVICEINFO" and not acquired:
                I = "PVRSRV_BRIDGE_IN_ACQUIRE_DEVICEINFO_TAG"
                self.op("acquire", g(I, r.inp, "uiDevIndex"), g(I, r.inp, "eDeviceType"))
                acquired = True
            elif r.name == "CREATE_DEVMEMCONTEXT" and not memctx:
                O = "PVRSRV_BRIDGE_OUT_CREATE_DEVMEMCONTEXT_TAG"
                self.memctx = g(O, r.out, "hDevMemContext")
                self.op("memctx")
                memctx = True
            elif r.name == "ALLOC_DEVICEMEM":
                I, O = "PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM_TAG", "PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM_TAG"
                b = by_seq[r.seq]
                i = len(self.bufs)
                self.bufs.append(b)
                hid = self.mm.heaps[b.heap].id & 0xFF
                self.heapid[i] = hid
                self.handles[b.kmeminfo] = f"m{i}"
                sync = g(O, r.out, "sClientSyncInfo.hKernelSyncInfo")
                if sync:
                    self.handles[sync] = f"s{i}"
                self.op("alloc", i, hid, self.hx(g(I, r.inp, "ui32Attribs")), self.hx(g(I, r.inp, "ui32Size")),
                        self.hx(g(I, r.inp, "ui32Alignment")), self.hx(b.devva))
                if b.cpu is not None:
                    self.op("map", i)
            elif r.name == "WRAP_EXT_MEMORY":
                O = "PVRSRV_BRIDGE_OUT_WRAP_EXT_MEMORY_TAG"
                b = by_seq[r.seq]
                i = len(self.bufs)
                self.bufs.append(b)
                self.heapid[i] = None
                self.handles[b.kmeminfo] = f"m{i}"
                sync = g(O, r.out, "sClientSyncInfo.hKernelSyncInfo")
                if sync:
                    self.handles[sync] = f"s{i}"
                size = g("PVRSRV_BRIDGE_IN_WRAP_EXT_MEMORY_TAG", r.inp, "ui32ByteSize")
                self.op("wrap", i, self.hx(size), self.hx(b.devva))
            elif r.name == "SGX_REGISTER_HW_RENDER_CONTEXT":
                I = "PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_RENDER_CONTEXT_TAG"
                O = "PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_RENDER_CONTEXT_TAG"
                ub = next(u for u in self.log.of(pvrlog.UBuf) if u.seq < r.seq and u.nr == r.nr and u.phase == 0
                          and u.ptr == g(I, r.inp, "pHWRenderContextCpuVAddr"))
                size = g(I, r.inp, "ui32HWRenderContextSize")
                assert len(ub.data) == size
                self.ctx_va = g(O, r.out, "sHWRenderContextDevVAddr.uiAddr")
                self.op("regctx", self.hx(self.blob(ub.data)), self.hx(size),
                        self.hx(g(I, r.inp, "ui32OffsetToPDDevPAddr")), self.hx(self.ctx_va))

    # -- memory ----------------------------------------------------------------
    def snapmem(self, kick):
        snaps = [s for s in self.log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == kick]
        return memmap.Memory(self.mm, snaps)

    def runs(self, words, base, pred):
        """Yield (first word index, end word index) of runs where pred(i) holds."""
        i, n = 0, len(words)
        while i < n:
            if not pred(i):
                i += 1
                continue
            j = i
            while j < n and pred(j):
                j += 1
            yield i, j
            i = j

    def write_run(self, va, d):
        self.op("write", self.hx(va), self.hx(len(d)), self.hx(self.blob(d)))

    def full(self, mem):
        nbytes = 0
        for i, b in enumerate(self.bufs):
            hid = self.heapid[i]
            if hid is None or hid in self.skip or b.cpu is None:
                continue
            d = mem.read(b.devva, b.size)
            if d is None:
                sys.exit(f"mkreplay: no snapshot for buffer {i} at {b.devva:#x}")
            self.op("zero", self.hx(b.devva), self.hx(b.size))
            n4 = len(d) // 4
            w = struct.unpack(f"<{n4}I", d[:n4 * 4])
            for s, e in self.runs(w, b.devva, lambda k: w[k] != 0):
                self.write_run(b.devva + 4 * s, d[4 * s:4 * e])
                nbytes += 4 * (e - s)
            if len(d) % 4 and any(d[n4 * 4:]):
                self.write_run(b.devva + n4 * 4, d[n4 * 4:])
        return nbytes

    def delta(self, prev, cur):
        nwords = 0
        for i, b in enumerate(self.bufs):
            hid = self.heapid[i]
            if hid is None or hid in self.dskip or b.cpu is None:
                continue
            a, c = prev.read(b.devva, b.size), cur.read(b.devva, b.size)
            n4 = len(c) // 4
            wa = struct.unpack(f"<{n4}I", a[:n4 * 4])
            wc = struct.unpack(f"<{n4}I", c[:n4 * 4])
            for s, e in self.runs(wc, b.devva, lambda k: wa[k] != wc[k]):
                self.op("#", f"buf {i} heap {hid}: {e - s} word(s) at +{4 * s:#x}")
                self.write_run(b.devva + 4 * s, c[4 * s:4 * e])
                nwords += e - s
        return nwords

    # -- kicks -----------------------------------------------------------------
    def kick(self, r):
        base = layout.layout(ARCH)["PVRSRV_BRIDGE_IN_DOKICK_TAG"].path("sCCBKick")[0]
        k = lambda p: g(K, r.inp, p, base)
        t = [f"ccb={self.handle(k('hCCBKernelMemInfo'))}", f"off={k('ui32CCBOffset'):#x}"]
        words = [k("sCommand.ui32ServiceAddress"), k("sCommand.ui32CacheControl")] + \
                [k(f"sCommand.ui32Data[{j}]") for j in range(6)]
        t.append("cmd=" + ",".join("ctx" if (j >= 2 and v and v == self.ctx_va) else f"{v:#x}"
                                   for j, v in enumerate(words)))
        if k("bFirstKickOrResume"):
            t.append("first")
        if k("bLastInScene"):
            t.append("last")
        if k("bTADependency"):
            t.append("tadep")
        t.append(f"hwsync={self.handle(k('hKernelHWSyncListMemInfo'))}")
        ndst = k("ui32NumDstSyncObjects")
        if ndst:
            ptr = k("pahDstSyncHandles")
            ub = [u for u in self.log.of(pvrlog.UBuf) if u.seq < r.seq and u.phase == 0 and u.ptr == ptr][-1]
            hs = struct.unpack(f"<{ndst}I", ub.data[:4 * ndst])
            self.last_dst = [self.handle(h) for h in hs]
            t.append("dst=" + ",".join(self.last_dst))
        for name, cnt, arr in (("ta", "ui32NumTAStatusVals", "asTAStatusUpdate"),
                               ("3d", "ui32Num3DStatusVals", "as3DStatusUpdate")):
            # clang does not expand arrays of records: SGX_INTERNEL_STATUS_UPDATE is
            # { u32 status dev addr, u32 status value, handle mem info } (12 bytes on i386)
            o = base + layout.layout(ARCH)[K].path(arr)[0]
            for j in range(k(cnt)):
                va, val, h = struct.unpack_from("<III", r.inp, o + 12 * j)
                t.append(f"{name}={va:#x}:{val:#x}:{self.handle(h)}")
        for j in range(k("ui32NumSrcSyncs")):
            t.append(f"src={self.handle(k(f'ahSrcKernelSyncInfo[{j}]'))}")
        for f, nm in (("hTA3DSyncInfo", "ta3dsync"), ("hTASyncInfo", "tasync"), ("h3DSyncInfo", "sync3d")):
            if k(f):
                t.append(f"{nm}={self.handle(k(f))}")
        if k("hDevMemContext") != self.memctx:
            sys.exit("mkreplay: kick on a memory context other than the first")
        self.op("kick", *t)

    def run(self):
        self.setup()
        kicks = [r for r in self.log.of(pvrlog.Bridge) if r.name == "SGX_DOKICK"]
        prev = None
        for n, r in enumerate(kicks, 1):
            mem = self.snapmem(n)
            if prev is None:
                self.op("#", "memory before kick 1 (kick-1 snapshot)")
                nb = self.full(mem)
                self.op("#", f"{nb} non-zero bytes")
            else:
                self.op("sleep", self.sleep_ms)
                self.op("#", f"words changed between kick {n - 1} and kick {n}")
                self.delta(prev, mem)
            self.op("sync")
            self.kick(r)
            prev = mem
        for h in getattr(self, "last_dst", []):
            self.op("waitsync", h, self.wait_ms)
        os.makedirs(self.out, exist_ok=True)
        with open(os.path.join(self.out, "script"), "w") as f:
            f.write("\n".join(self.lines) + "\n")
        with open(os.path.join(self.out, "data"), "wb") as f:
            f.write(self.data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("outdir")
    ap.add_argument("--skip-heaps", default="", help="heap ids not written before kick 1")
    ap.add_argument("--delta-skip-heaps", default="1,8,10", help="heap ids not written before later kicks")
    ap.add_argument("--dump", action="append", default=[], help="devva:size:file to dump at the end")
    ap.add_argument("--sleep", type=int, default=200, help="ms between kicks")
    ap.add_argument("--wait", type=int, default=2000, help="longest wait for the last kick's destination syncs, ms")
    a = ap.parse_args()
    ids = lambda s: {int(x, 0) for x in s.split(",") if x}
    gen = Gen(pvrlog.load(a.log), a.outdir, ids(a.skip_heaps), ids(a.delta_skip_heaps), a.sleep, a.wait)
    gen.run()
    with open(os.path.join(a.outdir, "script"), "a") as f:
        for d in a.dump:
            va, size, path = d.split(":", 2)
            f.write(f"dump {int(va, 0):#x} {int(size, 0):#x} {path}\n")
    print(f"mkreplay: {len(gen.lines)} ops, {len(gen.data)} data bytes, {len(gen.bufs)} buffers -> {a.outdir}")


if __name__ == "__main__":
    main()
