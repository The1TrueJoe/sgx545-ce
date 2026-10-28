"""Device memory and kick parameters from a pvrtrace log (a DDK capture, or
tools/gen's `frame -t` / `frame-fake -t`), for the emulator.

    for k in kicks(path):          # one per SGX_DOKICK / SGX_SUBMITTRANSFER
        k.kind, k.n                # "kick" / "xfer", 1-based
        k.load(mem)                # that event's snapshot into an emu.Mem
        k.data                     # sCommand.ui32Data[0..5]
        k.ccb_va, k.ccb_off        # the client CCB and the command's offset
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "decode"))
import layout  # noqa: E402
import memmap  # noqa: E402
import pvrlog  # noqa: E402


class Event:
    def __init__(self, kind, n, rec, mem, mm):
        self.kind, self.n, self.rec, self.mm = kind, n, rec, mm
        self.memory = mem
        L = layout.layout(mm.arch)
        if kind == "kick":
            base = L["PVRSRV_BRIDGE_IN_DOKICK_TAG"].path("sCCBKick")[0]

            def g(f):
                return layout.get(mm.arch, "_SGX_CCB_KICK_", rec.inp, f, base)
            self.data = [struct.unpack_from("<I", rec.inp, base + 8 + 4 * i)[0] for i in range(6)]
            hccb = g("hCCBKernelMemInfo")
            self.ccb_off = g("ui32CCBOffset")
            ccb = next(b for b in mm.bufs if b.kmeminfo == hccb)
            self.ccb_va = ccb.devva
            self.first = g("bFirstKickOrResume")
            self.last = g("bLastInScene")
        else:
            # SGX_SUBMITTRANSFER: the kernel's TRANSFER command has the HW transfer
            # context in Data[1]; the command sits in the context's CCB
            I = "PVRSRV_BRIDGE_IN_SUBMITTRANSFER_TAG"
            ctx = layout.get(mm.arch, I, rec.inp, "sKick.sHWTransferContextDevVAddr.uiAddr")
            self.data = [0, ctx, 0, 0, 0, 0]
            hccb = layout.get(mm.arch, I, rec.inp, "sKick.hCCBMemInfo")
            ccb = next(b for b in mm.bufs if b.kmeminfo == hccb)
            self.ccb_va = ccb.devva
            self.ccb_off = layout.get(mm.arch, I, rec.inp, "sKick.ui32SharedCmdCCBOffset")
            self.first = self.last = None

    def load(self, mem):
        for va, data, _ in self.memory.regions:
            mem.load(va, data)


def contexts(log, mm):
    """[(device address, bytes, PD offset)] of every registered HW render
    context (the kernel copies the client's buffer and fills in the PD)"""
    out = []
    I = "PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_RENDER_CONTEXT_TAG"
    O = "PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_RENDER_CONTEXT_TAG"
    for r in log.of(pvrlog.Bridge):
        if r.name == "SGX_REGISTER_HW_TRANSFER_CONTEXT":
            I = "PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_TRANSFER_CONTEXT_TAG"
            O = "PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_TRANSFER_CONTEXT_TAG"
            ptr = layout.get(mm.arch, I, r.inp, "pHWTransferContextCpuVAddr")
            ub = next(u for u in log.of(pvrlog.UBuf) if u.seq < r.seq and u.nr == r.nr and u.phase == 0
                      and (u.ptr & 0xFFFFFFFF) == ptr & 0xFFFFFFFF)
            out.append((layout.get(mm.arch, O, r.out, "sHWTransferContextDevVAddr.uiAddr"), ub.data,
                        layout.get(mm.arch, I, r.inp, "ui32OffsetToPDDevPAddr")))
            continue
        I = "PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_RENDER_CONTEXT_TAG"
        O = "PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_RENDER_CONTEXT_TAG"
        if r.name != "SGX_REGISTER_HW_RENDER_CONTEXT":
            continue
        ptr = layout.get(mm.arch, I, r.inp, "pHWRenderContextCpuVAddr")
        ub = next(u for u in log.of(pvrlog.UBuf) if u.seq < r.seq and u.nr == r.nr and u.phase == 0
                  and (u.ptr & 0xFFFFFFFF) == ptr & 0xFFFFFFFF)
        out.append((layout.get(mm.arch, O, r.out, "sHWRenderContextDevVAddr.uiAddr"), ub.data,
                    layout.get(mm.arch, I, r.inp, "ui32OffsetToPDDevPAddr")))
    return out


def kicks(path, ctxs=None):
    """the trace's kick and transfer events; `ctxs`, a list, receives contexts()"""
    log = pvrlog.load(path)
    mm = memmap.build(log)
    if ctxs is not None:
        ctxs += contexts(log, mm)
    out = []
    nk = nx = 0
    for r in log.of(pvrlog.Bridge):
        if r.name == "SGX_DOKICK":
            nk += 1
            kind, n = "kick", nk
        elif r.name == "SGX_SUBMITTRANSFER":
            nx += 1
            kind, n = "xfer", nx
        else:
            continue
        snaps = [s for s in log.of(pvrlog.Snap) if s.reason == kind and s.kick == n]
        out.append(Event(kind, n, r, memmap.Memory(mm, snaps), mm))
    return out
