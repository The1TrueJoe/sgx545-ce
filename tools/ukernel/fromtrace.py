"""Device memory and kick parameters from a pvrtrace log (a DDK capture, or
tools/gen's `frame -t` / `frame-fake -t`), for the emulator.

    for k in kicks(path):          # one per SGX_DOKICK / SGX_SUBMITTRANSFER
        k.kind, k.n                # "kick" / "xfer", 1-based
        k.load(mem)                # that event's snapshot into an emu.Mem
        k.data                     # sCommand.ui32Data[0..5]
        k.ccb_va, k.ccb_off        # the client CCB and the command's offset

    kicks(path, cleanups=True) adds the kernel's SGXMKIF_CMD_CLEANUP commands as
    kind "cleanup": k.data = [cleanup type, resource address, 0...], k.exit = True
    for those resman sends at process exit (sgxutils.c SGXCleanupRequest, resman.c)
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


SGXMKIF_CMD_CLEANUP = 5
CLEANUP_RT, CLEANUP_RC, CLEANUP_TC, CLEANUP_PB = 1, 2, 3, 5


class Cleanup:
    """a kernel CCB cleanup command (SGXMKIF_CMD_CLEANUP): no client memory"""
    kind = "cleanup"
    ccb_va = ccb_off = first = last = None

    def __init__(self, n, rec, ctype, res, exit=False):
        self.n, self.rec, self.exit = n, rec, exit
        self.data = [ctype, res, 0, 0, 0, 0]

    def load(self, mem):
        pass


def kicks(path, ctxs=None, cleanups=False):
    """the trace's kick and transfer events; `ctxs`, a list, receives contexts()"""
    log = pvrlog.load(path)
    mm = memmap.build(log)
    if ctxs is not None:
        ctxs += contexts(log, mm)
    out = []
    nk = nx = nc = 0
    live = {}       # (cleanup type, handle) -> resource address, registered and not yet unregistered
    for r in log.of(pvrlog.Bridge):
        if cleanups and r.name in ("SGX_REGISTER_HW_RENDER_CONTEXT", "SGX_REGISTER_HW_TRANSFER_CONTEXT"):
            t, o = ((CLEANUP_RC, "PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_RENDER_CONTEXT_TAG")
                    if "RENDER" in r.name else
                    (CLEANUP_TC, "PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_TRANSFER_CONTEXT_TAG"))
            f = "RenderContext" if t == CLEANUP_RC else "TransferContext"
            live[(t, layout.get(mm.arch, o, r.out, "hHW" + f))] = \
                layout.get(mm.arch, o, r.out, "sHW%sDevVAddr.uiAddr" % f)
            continue
        if cleanups and r.name in ("SGX_UNREGISTER_HW_RENDER_CONTEXT", "SGX_UNREGISTER_HW_TRANSFER_CONTEXT",
                                   "SGX_FLUSH_HW_RENDER_TARGET"):
            if r.name == "SGX_FLUSH_HW_RENDER_TARGET":
                t = CLEANUP_RT
                res = layout.get(mm.arch, "PVRSRV_BRIDGE_IN_SGX_FLUSH_HW_RENDER_TARGET_TAG", r.inp,
                                 "sHWRTDataSetDevVAddr.uiAddr")
            else:
                t, f = (CLEANUP_RC, "RenderContext") if "RENDER" in r.name else (CLEANUP_TC, "TransferContext")
                i = "PVRSRV_BRIDGE_IN_%s_TAG" % r.name
                if layout.get(mm.arch, i, r.inp, "bForceCleanup"):
                    continue        # forced: the kernel sends no command
                res = live.pop((t, layout.get(mm.arch, i, r.inp, "hHW" + f)), None)
                if res is None:
                    continue
            nc += 1
            out.append(Cleanup(nc, r, t, res))
            continue
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
    # process exit: resman frees render contexts, then transfer contexts, newest first
    # (no shared PB desc is ever registered here: the PB is per context)
    for t in (CLEANUP_RC, CLEANUP_TC):
        for (lt, _), res in reversed(list(live.items())):
            if lt == t:
                nc += 1
                out.append(Cleanup(nc, None, t, res, exit=True))
    return out
