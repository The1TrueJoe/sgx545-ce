"""Rebuild the GPU's view of memory from a pvrtrace log.

Chain per buffer:  ALLOC_DEVICEMEM (heap, size -> dev vaddr, hKernelMemInfo)
                -> MHANDLE_TO_MMAP_DATA (hKernelMemInfo -> mmap offset)
                -> mmap2 of the DRM node (offset -> CPU address)
so every snapshot (taken by CPU address) can be read by device address.
"""
import bisect
import struct
from dataclasses import dataclass, field

import layout
import pvrlog


@dataclass
class Heap:
    id: int
    handle: int
    base: int
    size: int
    attribs: int


@dataclass
class Buf:
    seq: int
    heap: int          # heap handle
    devva: int
    size: int
    attribs: int
    kmeminfo: int
    flags: int
    cpu: int = None
    map_len: int = 0
    mmap_off: int = None
    byte_off: int = 0
    origin: str = "alloc"

    def contains(self, va):
        return self.devva <= va < self.devva + self.size


@dataclass
class MemMap:
    arch: str
    heaps: dict = field(default_factory=dict)     # handle -> Heap
    bufs: list = field(default_factory=list)

    def heap_name(self, h):
        hp = self.heaps.get(h)
        return f"heap{hp.id:#x}" if hp else f"h{h:#x}"

    def find(self, va):
        for b in self.bufs:
            if b.contains(va):
                return b
        return None


HEAP_NAMES = {}  # filled in as heaps are identified (see notes/heaps.md)


def build(log):
    arch = "x86" if log.ptr == 4 else "x86_64"
    mm = MemMap(arch)
    by_kmi = {}
    pending_off = {}  # mmap offset -> Buf
    for r in log.recs:
        if isinstance(r, pvrlog.Bridge) and r.ret == 0:
            if r.name == "GET_DEVMEM_HEAPINFO":
                rec = layout.layout(arch)["PVRSRV_BRIDGE_OUT_GET_DEVMEM_HEAPINFO_TAG"]
                n = layout.u32(r.out, 4)
                hsz = layout.layout(arch)["_PVRSRV_HEAP_INFO_"].size
                base = rec.path("sHeapInfo")[0]
                for i in range(n):
                    o = base + i * hsz
                    hid = layout.get(arch, "_PVRSRV_HEAP_INFO_", r.out, "ui32HeapID", o)
                    h = layout.get(arch, "_PVRSRV_HEAP_INFO_", r.out, "hDevMemHeap", o)
                    mm.heaps.setdefault(h, Heap(
                        hid, h,
                        layout.get(arch, "_PVRSRV_HEAP_INFO_", r.out, "sDevVAddrBase.uiAddr", o),
                        layout.get(arch, "_PVRSRV_HEAP_INFO_", r.out, "ui32HeapByteSize", o),
                        layout.get(arch, "_PVRSRV_HEAP_INFO_", r.out, "ui32Attribs", o)))
            elif r.name == "ALLOC_DEVICEMEM":
                I, O = "PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM_TAG", "PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM_TAG"
                b = Buf(r.seq,
                        layout.get(arch, I, r.inp, "hDevMemHeap"),
                        layout.get(arch, O, r.out, "sClientMemInfo.sDevVAddr.uiAddr"),
                        layout.get(arch, O, r.out, "sClientMemInfo.uAllocSize"),
                        layout.get(arch, I, r.inp, "ui32Attribs"),
                        layout.get(arch, O, r.out, "sClientMemInfo.hKernelMemInfo"),
                        layout.get(arch, O, r.out, "sClientMemInfo.ui32Flags"))
                mm.bufs.append(b)
                by_kmi[b.kmeminfo] = b
            elif r.name == "WRAP_EXT_MEMORY":
                O = "PVRSRV_BRIDGE_OUT_WRAP_EXT_MEMORY_TAG"
                try:
                    b = Buf(r.seq, 0,
                            layout.get(arch, O, r.out, "sClientMemInfo.sDevVAddr.uiAddr"),
                            layout.get(arch, O, r.out, "sClientMemInfo.uAllocSize"), 0,
                            layout.get(arch, O, r.out, "sClientMemInfo.hKernelMemInfo"),
                            layout.get(arch, O, r.out, "sClientMemInfo.ui32Flags"), origin="wrap")
                    mm.bufs.append(b)
                    by_kmi[b.kmeminfo] = b
                except KeyError:
                    pass
            elif r.name == "MHANDLE_TO_MMAP_DATA":
                h = layout.get(arch, "PVRSRV_BRIDGE_IN_MHANDLE_TO_MMAP_DATA_TAG", r.inp, "hMHandle")
                O = "PVRSRV_BRIDGE_OUT_MHANDLE_TO_MMAP_DATA_TAG"
                off = layout.get(arch, O, r.out, "ui32MMapOffset")
                b = by_kmi.get(h)
                if b is None:
                    b = Buf(r.seq, 0, 0, 0, 0, h, 0, origin="mhandle-only")
                    mm.bufs.append(b)
                    by_kmi[h] = b
                b.mmap_off = off
                b.byte_off = layout.get(arch, O, r.out, "ui32ByteOffset")
                pending_off[off] = b
        elif isinstance(r, pvrlog.Mmap):
            # mmap2 offset is in pages; the services cookie is the page offset itself
            b = pending_off.get(r.off >> 12) or pending_off.get(r.off)
            if b is not None:
                b.cpu = r.addr + b.byte_off
                b.map_len = r.length
    return mm


class Memory:
    """Device-address reads over one set of snapshots (e.g. those taken at kick N)."""

    def __init__(self, mm, snaps):
        self.mm = mm
        self.by_cpu = {s.addr: s.data for s in snaps}
        self.regions = []  # (devva, bytes, Buf)
        for b in mm.bufs:
            if b.cpu is None:
                continue
            base = b.cpu - b.byte_off
            data = self.by_cpu.get(base)
            if data is None:
                continue
            self.regions.append((b.devva, data[b.byte_off:b.byte_off + b.size], b))
        self.regions.sort(key=lambda t: t[0])
        self._starts = [t[0] for t in self.regions]

    def region(self, va):
        i = bisect.bisect_right(self._starts, va) - 1
        if i >= 0:
            s, d, b = self.regions[i]
            if va < s + len(d):
                return s, d, b
        return None

    def read(self, va, n):
        r = self.region(va)
        if r is None:
            return None
        s, d, _ = r
        return d[va - s:va - s + n]

    def u32(self, va):
        d = self.read(va, 4)
        return None if d is None or len(d) < 4 else struct.unpack("<I", d)[0]

    def words(self, va, n):
        d = self.read(va, 4 * n)
        if d is None:
            return None
        return list(struct.unpack(f"<{len(d) // 4}I", d[:len(d) // 4 * 4]))
