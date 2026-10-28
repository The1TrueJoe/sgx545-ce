"""Turn a pvrtrace log into a script for tools/replay/replay (and its data file).

    python3 mkreplay.py <log.bin> <outdir> [--skip-heaps 8] [--delta-skip-heaps 1,8,10]

Authoring the frame instead of copying it (addresses default to the probe's
ref.bin capture; see docs/NOTES.md):
    --verts x0,y0,x1,y1,x2,y2   triangle positions (clip space), as floats
    --clear r,g,b,a             clear colour (0..1), the background object's ARGB8888
    --color r,g,b,a             the fragment uniform c, as FP16 pairs
    --usse VA:'insn; insn'      USSE instructions assembled by tools/isa/asm.py
    --poke VA:word,word         raw words
    --write VA:file             a file's bytes (e.g. a texture laid out by re/tex/tex.py)
Each override is poked after the snapshot is loaded and again before every kick.

Writes <outdir>/script (text, one op per line) and <outdir>/data (the bytes
the write ops point into). Buffers are named by their index in capture order
(allocations and wraps both count), handles as m<idx> (mem info) or s<idx>
(sync info), so the replay never sees a captured kernel handle.

Memory state before kick 1 is the kick-1 snapshot: each buffer outside the
skipped heaps is zeroed, then its non-zero runs are written. Before each later
kick only the words that changed since the previous kick's snapshot are written,
outside the delta-skipped heaps (the ones the GPU itself writes).

--xfers all (or a list of transfer numbers, 1-based) also replays the
SGX_SUBMITTRANSFERs (texture uploads, the swap blit), interleaved with the
kicks in capture order, and registers the transfer contexts. Each event's
memory comes from its own snapshot (pvrtrace snapshots before every
SUBMITTRANSFER); a buffer that did not exist yet at the previous event is
written in full. --no-delta VA:size keeps a range the GPU writes (the back
buffer before the swap, a texture after its upload) out of every delta, so the
replay's own GPU work is what ends up there.
"""
import argparse
import os
import struct
import sys

import layout
import memmap
import pvrlog

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "isa"))
import asm  # noqa: E402

ARCH = "x86"            # set from the log: i386 DDK captures, or x86_64 (tools/gen/frame -t)
K = "_SGX_CCB_KICK_"


def g(rec, buf, path, base=0):
    return layout.get(ARCH, rec, buf, path, base)


class Gen:
    def __init__(self, log, out, skip, dskip, sleep_ms, wait_ms):
        global ARCH
        self.log = log
        self.mm = memmap.build(log)
        ARCH = self.mm.arch
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
        self.pokes = []         # (dev addr, [words], comment) applied before every kick
        self.xfers = None       # None: no transfers; set(): all; else the transfer numbers to replay
        self.xctx = {}          # captured transfer context dev addr -> index
        self.nodelta = []       # (dev addr, size) never written by a delta
        self.peeks = []         # (label, dev addr, words) printed after every event
        self.writes = []        # (dev addr, bytes, comment) written before every kick

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
            elif r.name == "SGX_REGISTER_HW_TRANSFER_CONTEXT" and self.xfers is not None:
                I = "PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_TRANSFER_CONTEXT_TAG"
                O = "PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_TRANSFER_CONTEXT_TAG"
                ub = [u for u in self.log.of(pvrlog.UBuf) if u.seq < r.seq and u.nr == r.nr and u.phase == 0
                      and u.ptr == g(I, r.inp, "pHWTransferContextCpuVAddr")][-1]
                size = g(I, r.inp, "ui32HWTransferContextSize")
                va = g(O, r.out, "sHWTransferContextDevVAddr.uiAddr")
                x = self.xctx[va] = len(self.xctx)
                self.op("regxfer", x, self.hx(self.blob(ub.data[:size])), self.hx(size),
                        self.hx(g(I, r.inp, "ui32OffsetToPDDevPAddr")), self.hx(va))
            elif r.name == "SGX_REGISTER_HW_RENDER_CONTEXT":
                I = "PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_RENDER_CONTEXT_TAG"
                O = "PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_RENDER_CONTEXT_TAG"
                ub = next(u for u in self.log.of(pvrlog.UBuf) if u.seq < r.seq and u.nr == r.nr and u.phase == 0
                          and (u.ptr & 0xffffffff) == g(I, r.inp, "pHWRenderContextCpuVAddr") & 0xffffffff)
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
                if self.xfers is None:
                    sys.exit(f"mkreplay: no snapshot for buffer {i} at {b.devva:#x}")
                continue    # not mapped yet at a transfer: a later event writes it
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
            if c is None:
                continue
            if a is None:   # did not exist at the previous event: all of it
                a = bytes(len(c))
            n4 = len(c) // 4
            wa = struct.unpack(f"<{n4}I", a[:n4 * 4])
            wc = struct.unpack(f"<{n4}I", c[:n4 * 4])
            keep = [(va - b.devva, n) for va, n in self.nodelta]
            free = lambda k: not any(o <= 4 * k < o + n for o, n in keep)
            for s, e in self.runs(wc, b.devva, lambda k: wa[k] != wc[k] and free(k)):
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
            ub = [u for u in self.log.of(pvrlog.UBuf) if u.seq < r.seq and u.phase == 0 and (u.ptr & 0xffffffff) == ptr & 0xffffffff][-1]
            hw = 8 if ARCH == "x86_64" else 4
            hs = struct.unpack(f"<{ndst}{'Q' if hw == 8 else 'I'}", ub.data[:hw * ndst])
            self.last_dst = [self.handle(h) for h in hs]
            t.append("dst=" + ",".join(self.last_dst))
        for name, cnt, arr in (("ta", "ui32NumTAStatusVals", "asTAStatusUpdate"),
                               ("3d", "ui32Num3DStatusVals", "as3DStatusUpdate")):
            # clang does not expand arrays of records: SGX_INTERNEL_STATUS_UPDATE is
            # { u32 status dev addr, u32 status value, handle mem info } (12 bytes on i386)
            o = base + layout.layout(ARCH)[K].path(arr)[0]
            for j in range(k(cnt)):
                if ARCH == "x86_64":
                    va, val, h = struct.unpack_from("<IIQ", r.inp, o + 16 * j)
                else:
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

    def xfer(self, r):
        w = struct.unpack_from("<21I", r.inp, 0)
        k = w[2:]   # PVRSRV_TRANSFER_SGX_KICK (i386): ccb, off, ctx, tasync, 3dsync, nsrc, src[5], ndst, dst[5], flags
        t = [f"ccb={self.handle(k[0])}", f"off={k[1]:#x}", f"ctx={self.xctx[k[2]]}"]
        if k[3]:
            t.append(f"tasync={self.handle(k[3])}")
        if k[4]:
            t.append(f"sync3d={self.handle(k[4])}")
        if k[5]:
            t.append("src=" + ",".join(self.handle(h) for h in k[6:6 + k[5]]))
        if k[11]:
            self.last_xdst = [self.handle(h) for h in k[12:12 + k[11]]]
            t.append("dst=" + ",".join(self.last_xdst))
        if k[17]:
            t.append(f"flags={k[17]:#x}")
        self.op("xfer", *t)

    def rt_data(self, r, n):
        """The RT data set the last kick's TA command names (cmd +0x40)."""
        base = layout.layout(ARCH)["PVRSRV_BRIDGE_IN_DOKICK_TAG"].path("sCCBKick")[0]
        h = g(K, r.inp, "hCCBKernelMemInfo", base)
        off = g(K, r.inp, "ui32CCBOffset", base)
        ccb = next(b for b in self.bufs if b.kmeminfo == h)
        return self.snapmem(n).u32(ccb.devva + off - 0x58 + 0x40)

    def run(self):
        self.setup()
        kicks = [r for r in self.log.of(pvrlog.Bridge) if r.name == "SGX_DOKICK"]
        xfers = [r for r in self.log.of(pvrlog.Bridge) if r.name == "SGX_SUBMITTRANSFER"]
        events = [("kick", n, r) for n, r in enumerate(kicks, 1)]
        if self.xfers is not None:
            events += [("xfer", n, r) for n, r in enumerate(xfers, 1) if not self.xfers or n in self.xfers]
        events.sort(key=lambda e: e[2].seq)
        prev = None
        for kind, n, r in events:
            mem = memmap.Memory(self.mm, [s for s in self.log.of(pvrlog.Snap) if s.reason == kind and s.kick == n])
            if prev is None:
                self.op("#", f"memory before {kind} {n} (its snapshot)")
                nb = self.full(mem)
                self.op("#", f"{nb} non-zero bytes")
            else:
                self.op("sleep", self.sleep_ms)
                self.op("#", f"words changed up to {kind} {n}")
                self.delta(prev, mem)
            for va, d, why in self.writes:
                self.op("#", why)
                self.write_run(va, d)
            for va, words, why in self.pokes:
                self.op("#", why)
                self.op("poke", self.hx(va), *(f"{w:#010x}" for w in words))
            self.op("sync")
            if kind == "kick":
                self.kick(r)
            else:
                self.xfer(r)
            prev = mem
            if self.peeks:
                self.op("sleep", self.sleep_ms)
            for label, va, cnt in self.peeks:
                self.op("peek", f"{kind}{n}:{label}", self.hx(va), cnt)
        for h in getattr(self, "last_dst", []) + getattr(self, "last_xdst", []):
            self.op("waitsync", h, self.wait_ms)
        self.rtdata = self.rt_data(kicks[-1], len(kicks))
        os.makedirs(self.out, exist_ok=True)
        with open(os.path.join(self.out, "script"), "w") as f:
            f.write("\n".join(self.lines) + "\n")
        with open(os.path.join(self.out, "data"), "wb") as f:
            f.write(self.data)


def floats(s, n):
    v = [float(x) for x in s.split(",")]
    if len(v) != n:
        sys.exit(f"mkreplay: expected {n} numbers in {s!r}")
    return v


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def f16(x):
    return struct.unpack("<H", struct.pack("<e", x))[0]


def u8(x):
    return max(0, min(255, round(x * 255)))


def overrides(a):
    out = []
    if a.verts:
        v = floats(a.verts, 6)
        out.append((a.vb + 0x30, [f32(x) for x in v], f"triangle {a.verts}"))
    if a.clear:
        r, g, b, al = floats(a.clear, 4)
        argb = u8(al) << 24 | u8(r) << 16 | u8(g) << 8 | u8(b)
        for i in range(3):
            out.append((a.vb + 0x10 * i + 0xc, [argb], f"clear vertex {i} colour {a.clear}"))
    if a.color:
        r, g, b, al = floats(a.color, 4)
        out.append((a.uniform, [f16(r) | f16(g) << 16, f16(b) | f16(al) << 16], f"uniform c = {a.color}"))
    for spec in a.usse:
        va, prog = spec.split(":", 1)
        words = []
        for insn in [x for x in prog.split(";") if x.strip()]:
            hi, lo = asm.assemble(insn)
            words += [lo, hi]
        out.append((int(va, 0), words, f"USSE at {va}: {prog.strip()}"))
    for spec in a.poke:
        va, ws = spec.split(":", 1)
        out.append((int(va, 0), [int(w, 0) for w in ws.split(",")], f"poke {spec}"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("outdir")
    ap.add_argument("--skip-heaps", default="", help="heap ids not written before kick 1")
    ap.add_argument("--delta-skip-heaps", default="1,8,10", help="heap ids not written before later kicks")
    ap.add_argument("--dump", action="append", default=[], help="devva:size:file to dump at the end")
    ap.add_argument("--flushrt", action="store_true",
                    help="end with SGX_FLUSH_HW_RENDER_TARGET of the frame's RT data set, as the DDK does on "
                         "teardown (it timed out and locked the GPU up on a srvinit-started EA3)")
    ap.add_argument("--sleep", type=int, default=200, help="ms between kicks")
    ap.add_argument("--wait", type=int, default=2000, help="longest wait for the last kick's destination syncs, ms")
    ap.add_argument("--vb", type=lambda x: int(x, 0), default=0x3e51d000,
                    help="vertex buffer: clear object at +0 (3 x {x,y,z,ARGB}), the triangle's x,y at +0x30")
    ap.add_argument("--uniform", type=lambda x: int(x, 0), default=0x2e006184,
                    help="the uniform c in the PDS pixel data, FP16 (r,g) then (b,a)")
    ap.add_argument("--verts")
    ap.add_argument("--clear")
    ap.add_argument("--color")
    ap.add_argument("--usse", action="append", default=[])
    ap.add_argument("--poke", action="append", default=[])
    ap.add_argument("--write", action="append", default=[])
    ap.add_argument("--peek", action="append", default=[], help="LABEL:VA:words printed after every event")
    ap.add_argument("--no-delta", action="append", default=[], help="VA:size the deltas leave alone")
    ap.add_argument("--xfers", help="'all' or comma-separated transfer numbers to replay as well")
    a = ap.parse_args()
    ids = lambda s: {int(x, 0) for x in s.split(",") if x}
    gen = Gen(pvrlog.load(a.log), a.outdir, ids(a.skip_heaps), ids(a.delta_skip_heaps), a.sleep, a.wait)
    gen.pokes = overrides(a)
    gen.peeks = [(lb, int(va, 0), int(c, 0)) for lb, va, c in (p.split(":") for p in a.peek)]
    gen.nodelta = [tuple(int(x, 0) for x in d.split(":")) for d in a.no_delta]
    if a.xfers:
        gen.xfers = set() if a.xfers == "all" else {int(x) for x in a.xfers.split(",")}
    for spec in a.write:
        va, path = spec.split(":", 1)
        gen.writes.append((int(va, 0), open(path, "rb").read(), f"write {spec}"))
    gen.run()
    with open(os.path.join(a.outdir, "script"), "a") as f:
        for d in a.dump:
            va, size, path = d.split(":", 2)
            f.write(f"dump {int(va, 0):#x} {int(size, 0):#x} {path}\n")
        if a.flushrt:
            f.write(f"flushrt {gen.rtdata:#x}\n")
    print(f"mkreplay: {len(gen.lines)} ops, {len(gen.data)} data bytes, {len(gen.bufs)} buffers -> {a.outdir}")


if __name__ == "__main__":
    main()
