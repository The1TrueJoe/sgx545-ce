# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""An open SGX545 USSE microkernel, in assembly.

The open replacement for Imagination's EDM microkernel, written from the
behavioural spec in `docs/UKERNEL.md` — what the DDK microkernel *does*,
reproduced in our own code and layout, never translated from its bytes.

`build.py` turns this into the binary `srvinit` loads and a manifest of entry
offsets. Everything is checked in the offline emulator (`tools/ukernel/`,
`ukernel/test_emu.py`, `ukernel/run_ours.py`) before any hardware run.

Design (one render context; the minimum the Mesa driver uses). The DDK keeps a
general multi-render scheduler; ours is the straight path a single client's
frame takes:

  host kick (CCB command, type in ServiceAddress) --> dispatch
      type 0 (TA)        --> program + start the TA, remember the command
  TA-finished event      --> write TA status, program + start the 3D
  3D-end-of-render event --> update the destination sync objects, host IRQ

State that must outlive a task is written to the TA3D control block in device
memory (addressed through `sa0`, its own base) so the next task's DMA reloads
it. Our scratch words (control block + 0x30..) carry the command and
render-details pointers from the TA to the 3D and the completion.

The register tables below (reg byte-offset <- base word index) are the
per-register provenance from docs/UKERNEL.md §7, i.e. exactly which structure
word the DDK loads each core register from. Reproducing them is what makes our
microkernel program the cores identically for a frame; `run_ours.py` diffs our
writes against the DDK's.
"""
import os

DEBUG_SENTINEL = os.environ.get("UK_SENTINEL") == "1"   # chain markers +0xDC..+0xE8
XFER_BANK_ALL = os.environ.get("UK_XFER_BANK") == "1"   # blit variant B: route
                                             # ALL of the transfer's data masters (TA/HOST/3D/2D)
                                             # to list 1 (the ctx PD), not just TA/3D. The
                                             # transfer pixel task may run on the 2D/HOST DM, whose
                                             # BANK0 nibble we leave at list 0 (idle) -> USSE-code
                                             # fetch at 0x37000000 faults. Safe on
                                             # the CE5300 (adds routing a 3D-DM transfer never uses).
RT_LOG = os.environ.get("UK_RTLOG") == "1"   # interleave detector (both words inside the devcd
                                             # dump): word 0x37 = the open scene's RT, word 0x3a
                                             # = 0x1217 when a first kick opens a scene on a
                                             # different RT while a render is still in flight (the
                                             # render-to-texture case the DDK's 2nd DPM slot
                                             # covers and ours does not). Default off -> no bytes.
# CCB-trace diagnostic: split lost-wakeup vs TA2-hangs-valid. Words
# that reach the dump (0x00-0x3b) and don't collide with live state (0x36 =
# SC_TAFLIGHT): 0x35 do_ta count, 0x37 last do_ta client RO, 0x38 last cmd+0x04,
# 0x39 last cmd+0x58 (kernel ready flag), 0x3a/0x3b kernel WO/RO at kick_end.
CCB_TRACE = os.environ.get("UK_CCBTRACE") == "1"
# Power-up CR log: after srvinit ours goes idle -> ACTIVE_POWER ->
# the core powers off; frame 1's kick powers it back up via SGXReset's soft-reset-
# only (power-up) branch, which does NOT restore CRs to srvinit's init values, only
# to hardware defaults. Log the candidate CRs at do_ta's FIRST kick (core powered)
# to the dump words so a cold frame-1 lockup shows which CR came up wrong.
CR_PWRLOG = os.environ.get("UK_PWRLOG") == "1"
# Kick-2 (terminate) entry log: log the state TA1 / its finish left,
# just before the terminate's 0x200 kick, to the dump words. The key one is 0x118
# EVENT_STATUS2 — if TRIG_TA (0x80) or an OOM bit is ALREADY set before our kick,
# the hang comes from TA1 or its finish, not kick-2's programming.
K2_LOG = os.environ.get("UK_K2LOG") == "1"
# DPM trajectory log: locate WHERE the terminate's TA DPM pages are
# wiped (K2LOG showed 0x724/0x72c/0x700 = 0 at the terminate). 0x700 (the readback
# of 0x61c = 0x04210000, so ~0x0421xxxx when live) at three kick-1 points, 0x724 at
# TA1's finish, 0x740 at kick-1 entry, and the NumActivePowerEvents delta
# (hostctl[11]) between kick 1 and the terminate. The decision tree: A=0 the
# free-list load isn't taking (upstream); A!=0,B=0 dpm_load overwrites it; B!=0,C=0
# TA1 closes the scene; C!=0 & 0 at terminate = wiped between (0x3b>0 = power-off).
DPM_LOG = os.environ.get("UK_DPMLOG") == "1"
# EVENT_TIMER diagnostic: the 0xacc enable didn't stop the watchdog HWR
# (14/30). Log at init hostctl[6] (ui32uKernelTimerClock, the period), the 0xacc value
# we wrote (hostctl[6]|ENABLE), and the 0xacc read-back. hostctl[6]=0 => the driver's
# SGXUpdateTimingInfo didn't populate it (write a real period instead); nonzero =>
# the timer fires but a bare event pass doesn't move 0xb14 (needs a real handler).
TMR_LOG = os.environ.get("UK_TMRLOG") == "1"
# Wait-site / hang-stage log: a render hangs silently (the
# ticking EDM hides it) and ~2.2 s later one of our self.wait() poll loops spins
# forever on a done bit, freezing the EDM — that is the HWR the watchdog catches.
# Find WHICH wait and WHICH stage. Words: 0x35 = the live wait-site id (nonzero = a
# wait is spinning, names it); per timer tick (last wins) 0x37 = EVENT_STATUS,
# 0x38 = EVENT_STATUS2, 0x39 = 0x008 CLKGATESTATUS, 0x3a = stage counters (byte each:
# do_ta/ta_finished/do_3d/render_done), 0x3b = tick counter.
WAIT_LOG = os.environ.get("UK_WAITLOG") == "1"
# Frame profile: UK_FRAMEPROF=1 stamps the EDM tick counter at the four render
# phases so the host can split a frame into TA (vertex) vs 3D (pixel). The counter (ctlblk 0x3b) increments on
# every EDM timer tick (~1 kHz keep-alive = ~1 ms); each phase copies it to its own ctlblk word, host-readable
# via the regsample over BAR0. Debug-only, off by default (byte-identical production image). Words (ctlblk+):
#   0x3b = tick, 0x37 = TA kick, 0x38 = TA finished, 0x39 = 3D kick, 0x3a = 3D finished. (Overlaps TMR/WAIT_LOG
# words, which are separate build flags.) ~1 ms resolution; a free-running GPU counter would give sub-ms if one
# proves readable.
FRAMEPROF = os.environ.get("UK_FRAMEPROF") == "1"
FP_TICK = 0x3b
FP_TA_KICK, FP_TA_FIN, FP_3D_KICK, FP_3D_FIN = 0x37, 0x38, 0x39, 0x3a
# Running totals (words 0x3d-0x3f are free; 0x40 is hostctl). Two host reads 5 s apart give the averages
# over ~50 frames so the 1 ms tick error averages out and regsample needn't catch each render. (Zeroed at every
# microkernel start; survive within a session while the GPU stays up.)
FP_TA_BUSY, FP_3D_BUSY, FP_COUNTS = 0x3d, 0x3e, 0x3f   # TA-busy ticks, 3D-busy ticks, TA count (low 16)|3D (high 16)
# TA/3D OVERLAP: run job N+1's TA while job N's 3D is in flight (the DDK does; Minecraft
# TA ~42 ms + 3D ~26 ms, so ~11 -> ~20 fps). Off by default -> serial production image byte-identical. The
# serial path keeps SC_RTENTRY/SC_RD/SC_CTX as the TA set (the current binning scene, so the OOM
# code is untouched); overlap ADDS a 3D set populated at the 3D kick and read by render_complete, so the TA can
# advance to N+1 while N's 3D completes. Slot pick at the first kick: bind the slot the in-flight 3D is NOT on
# (0x630 b2=TA slot, b1=3D slot, b0=first-kick slot), like the DDK (interleave_ours.py --order overlap). 3D is
# strictly one at a time (a 3D queue in render_complete). The kernel driver credits 2 behind `overlap`.
OVERLAP = os.environ.get("UK_OVERLAP") == "1"
TD_ENTRY = 0x19      # control block +0x64: the in-flight 3D's RT-data entry (0 = no 3D). Set at the 3D kick,
                     # cleared at the DPM free. The slot pick reads it ("is a 3D in flight, and on 0x630 b1").
TD_RD = 0x1c         # control block +0x70: the in-flight 3D's render details. render_complete reads it instead
                     # of SC_RD, because by completion the TA set may already be N+1's. (DDK uses ctx +0x28/+0x2c.)
TD_CTX = 0x20        # control block +0x80: the in-flight 3D's HW render context (for the completion teardown's
                     # ctx in-flight clears). Populated at the 3D kick with TD_ENTRY/TD_RD.
PENDING_3D = 0x26    # control block +0x98: set when a scene's last TA finished but a 3D was already in flight,
                     # so its 3D is QUEUED (never two 3Ds). render_complete starts it after the current 3D frees.
# EVENT_TIMER period divisor for the perf measurement: UK_TIMERDIV=N makes the EDM keep-
# alive timer N times slower (1 = default 1 kHz, 4 = 250 Hz, 10 = 100 Hz). Scales the
# host-set period at init and at power-resume. Default 1 emits nothing -> byte-identical.
TIMER_DIV = int(os.environ.get("UK_TIMERDIV", "1"))

# M1-exit experiment (the frame-40 hang): the idle ACTIVE_POWER
# raise in kick_end strands the NEXT host doorbell after the CCB drains on a single context
# (per-context, idle-timer driven; 20 processes never idle long enough; DDK clean). Set
# UK_NO_IDLE_AP=1 to skip the raise (keep only the EVENT_TIMER keep-alive) and A/B it on HW
# -- pair with a lengthened/disabled SGXOSTimer watchdog driver-side. Default 0 is
# byte-identical (the raise is still emitted).
NO_IDLE_AP = int(os.environ.get("UK_NO_IDLE_AP", "0"))

# ---- per-core configuration (docs/UKERNEL-TOOLING.md §3) -------------------
# Register map, control-block layout, host-control/command offsets and the
# register-programming tables for the core this image targets, loaded from
# cores/<core>.py. The core is SGX_CORE (default sgx545); build.py sets it from
# its optional leading argument. The handler logic below is core-independent.
# Byte-identity gate for sgx545: ukernel/golden/sgx545.golden.
import importlib  # noqa: E402

CORE = os.environ.get("SGX_CORE", "sgx545")
_core = importlib.import_module("cores." + CORE)
globals().update({k: v for k, v in vars(_core).items() if not k.startswith("_")})
del _core
# the directory list a job's page directory goes in: list 1 with BANK0 routing, or
# on a one-context core the only list, switched per job and restored after it
CTX_DIR_LIST = CR_BIF_DIR_LIST_BASE1 if MULTI_MEM_CONTEXTS else CR_BIF_DIR_LIST_BASE0
if OVERLAP and not MULTI_MEM_CONTEXTS:
    raise SystemExit(f"UK_OVERLAP needs MULTI_MEM_CONTEXTS ({CORE} has one memory context)")


class Asm:
    def __init__(self):
        self.lines = []
        self.waits = []           # (id, poll, mask) of each bounded wait (WAIT_BOUND)

    def label(self, name):
        self.lines.append(("label", name))

    def i(self, text):
        self.lines.append(("i", text))

    def raw(self, hi, lo):
        """emit a raw instruction word (hi/lo 32-bit halves) the text assembler
        can't encode -- the USE/EDM cache ops op0xff/op0x100 (ISA.md undecoded,
        the DDK's data-cache flush/invalidate). hw.py decodes them; asm.py can't
        emit them yet, so we inject the exact words read from the DDK."""
        self.lines.append(("raw", (hi, lo)))

    def patch(self, reg, name):
        """a limm the srvinit loader fills with a device address at load time
        (the DDK's own mechanism for dynamic addresses). build.py records its
        offset and `name` in the manifest; the instruction is `limm rN, #0xdbeef`
        (the 0xdbeef sentinel powervr_sgx's loader checks before patching)."""
        self.lines.append(("patch", (reg, name)))

    # --- building blocks -----------------------------------------------
    def end(self):
        # NOTE: the DDK task end also runs op0x100 #3,#0,#3 + idf/wdf/
        # phas here (a USE/EDM cache flush). Adding that whole sequence to every
        # a.end() REGRESSED: the microkernel completed init
        # (srvinit rc=0) but then stuck at a task end, never processing kicks
        # (do_3d never reached, no lockup bit) -- likely the phas.i [wait=7] or the
        # flush hanging in our event context. Left out pending guidance
        # on placement (only the worker task end? no phas? different op).
        self.i("nop [end]")

    def phas(self):
        self.i("phas.i #0, #0 [wait=7]")

    def dispatch(self, bit, label):
        """if EUR_CR_EVENT_STATUS (in r2) has `bit` set, branch to `label`"""
        self.i(f"limm r3, #{bit}")
        self.i("and r4, r2, r3")
        self.i("test.and.ne p0, r4, r4")
        self.i(f"(p0) ba {label}")

    def ld_sa(self, dst, saword, off):
        """dst = *(*(ctlblk+saword) + off); saword 0 (SA_SELF) is the base (r17)
        itself. The base is a reserved temp, not a secondary attribute, because
        sa is unset in the bare-DOUTU event context until the emit/DMA sets it up
        (docs/UKERNEL.md §1)."""
        if saword == SA_SELF:
            self.i(f"ld.a.d r{dst}, r17, #{off} [moe]")
        else:
            self.i(f"ld.a.d r{dst}, r17, #{saword} [moe]")
            self.i("wdf")
            self.i(f"ld.a.d r{dst}, r{dst}, #{off} [moe]")
        self.i("wdf")

    def st_sa(self, saword, off, src):
        """*(*(ctlblk+saword) + off) = r<src>; saword 0 (SA_SELF) is the base r17"""
        if saword == SA_SELF:
            self.i(f"st.a.d r17, #{off}, r{src} [moe]")
        else:
            self.i(f"ld.a.d r0, r17, #{saword} [moe]")
            self.i("wdf")
            self.i(f"st.a.d r0, #{off}, r{src} [moe]")
        self.i("idf")
        self.i("wdf")

    def program(self, base_reg, table):
        """for each (reg, word): r4 = *(base_reg + word); reg = r4.
        The load offset field is 7 dwords wide, so when a table's offsets run
        past 127 the pointer is rebased by `limm`+`imae` first (as the DDK
        does), and the loads use offsets relative to that base."""
        minw = min(e[1] for e in table)
        rebase = max(e[1] for e in table) > 127
        src = base_reg
        if rebase:
            self.i(f"limm r3, #{minw * 4}")
            self.i(f"imae r3, r3, #1, r{base_reg} [t2=u32]")
            src = 3
        for entry in table:
            reg, word = entry[0], entry[1]
            self.i(f"ld.a.d r4, r{src}, #{word - (minw if rebase else 0)} [moe]")
            self.i("wdf")
            if len(entry) > 2:                       # a transform: OR a mode bit in
                self.i(f"limm r5, #{entry[2]}")
                self.i("or r4, r4, r5")
            self.i(f"str #{reg // 4}, r4")

    def consts(self, table):
        """write a fixed immediate to each register"""
        for reg, val in table:
            self.i(f"limm r4, #{val}")
            self.i(f"str #{reg // 4}, r4")

    _wl = 0

    def wait(self, poll, mask, ack):
        """spin until (poll & mask) == mask, then write mask to the ack register.
        The mask goes in a register (test immediates are only 7 bits)."""
        Asm._wl += 1
        lbl = f"_wait{Asm._wl}"
        if WAIT_LOG:                                   # name this wait while it spins
            self.i(f"limm r5, #{Asm._wl}")
            self.st_sa(SA_SELF, 0x35, 5)
        self.i(f"limm r6, #{mask}")
        if WAIT_BOUND:
            self.waits.append((Asm._wl, poll, mask, "set"))
            self.i(f"limm r15, #{WAIT_BOUND}")      # r15: only the bounded wait uses it
        self.label(lbl)
        self.i(f"ldr r5, #{poll // 4}")
        self.i("wdf")
        self.i("and r5, r5, r6")
        self.i(f"test.xor.ne p0, r5, r6")
        if WAIT_BOUND:
            # an unverified core: a bit it lacks must not freeze the EDM. Give up,
            # name the wait in the control block, count it, and carry on.
            self.i(f"(!p0) ba {lbl}_ok")
            self.i("test.isubu16.ne p0, r15, r15, #1")
            self.i(f"(p0) ba {lbl}")
            self.timed_out(Asm._wl)
            self.label(f"{lbl}_ok")
        else:
            self.i(f"(p0) ba {lbl}")
        self.i(f"str #{ack // 4}, r6")
        if WAIT_LOG:                                   # wait returned -> clear the id
            self.i("limm r5, #0")
            self.st_sa(SA_SELF, 0x35, 5)

    def timed_out(self, wid):
        """a bounded wait gave up: name it and count it in the control block (r5)"""
        self.i(f"limm r5, #{wid}")
        self.i(f"st.a.d r17, #{WAIT_TIMEOUT_ID}, r5 [moe]")
        self.i(f"ld.a.d r5, r17, #{WAIT_TIMEOUT_CT} [moe]")
        self.i("wdf")
        self.i("imae r5, r5, #1, #1 [t2=u32]")
        self.i(f"st.a.d r17, #{WAIT_TIMEOUT_CT}, r5 [moe]")
        self.i("idf")
        self.i("wdf")

    def job_pd(self, dst, ctx):
        """dst = the PD a one-context core loads for the HW context in r<ctx>. The
        context's CTX_PD word is a slot in the host's read-only PD table (r14), not
        an address, masked to the table so any value names a host-written entry."""
        self.i(f"ld.a.d r{dst}, r{ctx}, #{CTX_PD} [moe]")
        self.i("wdf")
        self.i(f"limm r5, #{PD_TABLE_SLOTS - 1}")
        self.i(f"and r{dst}, r{dst}, r5")
        self.i(f"imae r{dst}, r{dst}, #4, r14 [t2=u32]")
        self.i(f"ld.a.d r{dst}, r{dst}, #0 [moe]")
        self.i("wdf")

    def bif_settle(self):
        """BRN_28889: after a BIF directory invalidate, let the BIF drain before any
        data invalidate follows: read BIF_CTRL back, then spin (bounded) until
        BIF_MEM_REQ_STAT shows no request outstanding. Uses r5, r15, p0."""
        Asm._wl += 1
        lbl = f"_wait{Asm._wl}"
        self.waits.append((Asm._wl, CR_BIF_MEM_REQ_STAT, BIF_MEM_REQ_MASK, "clear"))
        self.i(f"ldr r5, #{CR_BIF_CTRL // 4}")
        self.i("wdf")
        self.i(f"limm r15, #{WAIT_BOUND or 0xffff}")
        self.label(lbl)
        self.i(f"ldr r5, #{CR_BIF_MEM_REQ_STAT // 4}")
        self.i("wdf")
        self.i(f"limm r6, #{BIF_MEM_REQ_MASK}")
        self.i("and r5, r5, r6")
        self.i("test.and.eq p0, r5, r5")
        self.i(f"(p0) ba {lbl}_ok")
        self.i("test.isubu16.ne p0, r15, r15, #1")
        self.i(f"(p0) ba {lbl}")
        self.timed_out(Asm._wl)
        self.label(f"{lbl}_ok")

    _rl = 0

    def restore_fw_pd(self):
        """one-context cores: DIR_LIST_BASE0 = the firmware PD (PD table slot 0),
        with an INVALDC, unless it already holds it. Uses r4, r5, p0."""
        Asm._rl += 1
        lbl = f"_fwpd{Asm._rl}"
        self.i("ld.a.d r4, r14, #0 [moe]")
        self.i(f"ldr r5, #{CR_BIF_DIR_LIST_BASE0 // 4}")
        self.i("wdf")
        self.i("test.xor.eq p0, r4, r5")
        self.i(f"(p0) ba {lbl}")
        self.i(f"str #{CR_BIF_DIR_LIST_BASE0 // 4}, r4")
        self.i(f"limm r5, #{BIF_CTRL_INVALDC}")
        self.i(f"str #{CR_BIF_CTRL // 4}, r5")
        self.i("limm r5, #0")
        self.i(f"str #{CR_BIF_CTRL // 4}, r5")
        self.label(lbl)

    def invalidate(self, inv_reg, done_mask):
        """request a BIF/USE cache invalidate and wait for it (docs/UKERNEL.md
        §6). inv_reg is the PDS invalidate for this side (TA 0xad0, 3D 0xad4);
        the MADD and USE caches are pulsed too, and their done bits waited on."""
        self.i(f"limm r5, #1")
        self.i(f"str #{inv_reg // 4}, r5")             # PDS cache invalidate
        self.i(f"str #{CR_PDS_INV_CSC // 4}, r5")      # coefficient-store invalidate
        self.i("limm r5, #268435456")                  # 0x10000000: MADD cache
        self.i(f"ldr r4, #{0x804 // 4}")
        self.i("wdf")
        self.i("or r4, r4, r5")
        self.i(f"str #{0x804 // 4}, r4")
        self.i("limm r5, #1")
        self.i(f"str #{CR_USE_INVAL // 4}, r5")        # USE cache invalidate
        self.wait(0x138, done_mask, CR_EVENT_HOST_CLEAR_0x140)
        self.wait(CR_EVENT_STATUS, 0x04000000, CR_EVENT_HOST_CLEAR)   # MADD done
        # NOTE: the DDK also runs op0xff #side (USE/EDM cache op) here after the MADD
        # ack. Tried op0xff #0 on the TA side: no improvement (10/30
        # frames, still intermittent lockup_TA) — the TA-cache invalidate is not the
        # terminate-kick cause. See the op0x100 flush at ta_finished's tail instead.

    def invalidate3d(self):
        """DDK-ordered 3D-side cache invalidate,
        used on the overlap queued 3D route. Unlike invalidate(), the MADD cache
        invalidate is pulsed and WAITED+acked FIRST, and only then are the PDS-3D /
        coefficient-store / USE invalidates requested, immediately before the
        0x138 == 0x4b spin. In the overlap queued route do_3d runs from
        render_complete with the 3D units clock-gated off (the hang symptom:
        CLKGATESTATUS 0x20, EVENT_STATUS2 0 — the 0x428 kick never ran); the MADD pulse wakes the
        units, and the PDS-side 0x4b done bits can only set once it has retired.
        invalidate()'s order (PDS first, 0x4b spin BEFORE the MADD wait) wins in the
        serial case because the units are still warm from the just-finished TA, but
        on the queued route it spins on 0x4b before the units are up and deadlocks."""
        self.i("limm r5, #268435456")                  # 0x10000000: MADD cache
        self.i(f"ldr r4, #{0x804 // 4}")
        self.i("wdf")
        self.i("or r4, r4, r5")
        self.i(f"str #{0x804 // 4}, r4")
        self.wait(CR_EVENT_STATUS, 0x04000000, CR_EVENT_HOST_CLEAR)   # MADD done (b26), ack 0x134
        self.i("limm r5, #1")
        # No 0xa08 here. The early BIF block (do_3d, right after 0x630->7) already made the
        # USE/PDS-code request ONCE this run (DDK r22 |= 0x40); the DDK's late 3D handshake sees
        # that flag and skips its own 0xa08, requesting only 0xad4/0xae0 and collecting the early
        # request's done bits (0x03, part of 0x4b) inside the 0x4b spin. Requesting 0xa08 twice
        # in one run isn't what the DDK does. 3D path only; serial invalidate() stands.
        self.i(f"str #{CR_PDS_INV1 // 4}, r5")         # 0xad4 PDS 3D
        self.i(f"str #{CR_PDS_INV_CSC // 4}, r5")      # 0xae0 coefficient store
        self.wait(0x138, INV_DONE_3D, CR_EVENT_HOST_CLEAR_0x140)      # spin 0x4b (incl. 0x03 from the early 0xa08), ack 0x140

    def ta_settle(self, tag):
        """TA must already be paused (0xa74 b0). Spin-read 0x700 until two reads 400
        iterations apart match, so a following free-list reload reads a stable counter
        (as the DDK does). Uses r5/r6/r7, p2; `tag` names the two labels."""
        self.label(f"{tag}_settle")
        self.i(f"ldr r5, #{0x700 // 4}"); self.i("wdf"); self.i("limm r6, #400")
        self.label(f"{tag}_spin")
        self.i("test.isubu16.ne p2, r6, r6, #1"); self.i(f"(p2) ba {tag}_spin")
        self.i(f"ldr r7, #{0x700 // 4}"); self.i("wdf")
        self.i("test.xor.ne p2, r7, r5"); self.i(f"(p2) ba {tag}_settle")

    def ta_freelist_reload(self):
        """Re-seed the TA free list from the live counters (as the DDK does): 0x61c=0x700,
        0x648=0x710<<16, 0x638=(0x724>>16)|((0x72c>>16)<<16), 0x684=1, wait EVENT_STATUS2 b0.
        Call with the TA paused + settled. Uses r4/r5."""
        self.i(f"ldr r4, #{0x700 // 4}"); self.i("wdf"); self.i(f"str #{0x61c // 4}, r4")
        self.i(f"ldr r4, #{0x710 // 4}"); self.i("wdf"); self.i("shl r4, r4, #16"); self.i(f"str #{0x648 // 4}, r4")
        self.i(f"ldr r4, #{0x724 // 4}"); self.i("wdf"); self.i("shr r4, r4, #16")
        self.i(f"ldr r5, #{0x72c // 4}"); self.i("wdf"); self.i("shr r5, r5, #16"); self.i("shl r5, r5, #16")
        self.i("or r4, r4, r5"); self.i(f"str #{0x638 // 4}, r4")
        self.i("limm r5, #1"); self.i(f"str #{CR_DPM_TA_FL // 4}, r5")
        self.wait(CR_EVENT_STATUS2, 0x1, CR_EVENT_HOST_CLEAR2)

    def dpm_freelist(self, mode, kick_reg, done_bit):
        """load a DPM free list so the geometry/render can allocate parameter-
        buffer pages: selector 0x664 = `mode` (TA 3, 3D 5), then pulse the TA
        (0x684) or 3D (0x680) load register, and wait for the done event in
        EUR_CR_EVENT_STATUS2 (TA bit 0, 3D bit 1). Without this the free list is
        empty, the parameter buffer's pages-in-use stays 0 and the TA never runs
        (docs/UKERNEL.md §6 "Load the free-list")."""
        self.i(f"limm r5, #{mode}")
        self.i(f"str #{CR_DPM_FL_MODE // 4}, r5")
        self.i("limm r5, #1")
        self.i(f"str #{kick_reg // 4}, r5")
        self.wait(CR_EVENT_STATUS2, done_bit, CR_EVENT_HOST_CLEAR2)

    def dpm_load(self):
        """load the DPM state and control, and clear the OTPM, waiting for each
        (docs/UKERNEL.md §6) so the free list is ready before the TA kicks."""
        self.i("limm r5, #2")
        self.i(f"str #{CR_DPM_STATE // 4}, r5")        # DPM state load
        self.i(f"str #{CR_DPM_CONTROL // 4}, r5")      # DPM control load
        self.wait(CR_EVENT_STATUS, 0x240, CR_EVENT_HOST_CLEAR)
        self.i("limm r5, #9")
        self.i(f"str #{CR_OTPM // 4}, r5")             # OTPM invalidate + clear
        for poll, mask, ack in OTPM_CLEAR_WAITS:
            self.wait(poll, mask, ack)


def build(a):
    # ======================================================================
    # event entry: the one task the event program starts, which dispatches
    # ======================================================================
    # The event program is a bare DOUTU (the DDK's is too; an event-kicked PDS
    # program is not relied on to run a DMA), so `sa` is empty here. This entry
    # loads the control block base from a loader-patched limm and populates the
    # four secondary attributes the handlers use (self/host-control/CCB-control/
    # kernel-CCB), the way the DDK's pds1 DMA would, then dispatches on whether
    # init has run and on EUR_CR_EVENT_STATUS (docs/UKERNEL.md §2). Every
    # handler is reached from here by a branch (one task), so only this entry
    # has the phase/end wrapper.
    a.label("event")
    if DIAG_ENTRY is not None:
        # bring-up only (cores/sgx530_diag.py): markers before phas.i. A register
        # store, then memory: two stores, a ld/add/st, then the normal entry (+1 again)
        a.i(f"limm r0, #{DIAG_TIMER}")
        a.i(f"str #{CR_EVENT_TIMER // 4}, r0")
        a.patch(17, "ctlblk")
        if not MULTI_MEM_CONTEXTS:
            a.patch(14, "pdtable")
        a.i(f"limm r0, #{DIAG_ENTRY}")
        a.i(f"st.a.d r17, #{WAIT_TIMEOUT_ID}, r0 [moe]")
        a.i("idf")
        a.i("wdf")
        a.i(f"ldr r1, #{CR_CORE_ID // 4}")
        a.i("wdf")
        a.i(f"st.a.d r17, #{SC_VERSION}, r1 [moe]")   # CORE_ID; init overwrites it
        a.i(f"ld.a.d r2, r17, #{HEARTBEAT} [moe]")
        a.i("wdf")
        a.i("imae r2, r2, #1, #1 [t2=u32]")
        a.i(f"st.a.d r17, #{HEARTBEAT}, r2 [moe]")
        a.i("idf")
        a.i("wdf")
    if DIAG_ENTRY is None or DIAG_PHAS:
        a.phas()
    # (A `lock #1`/`release #1` task mutex was tried here + in a.end() to serialise
    # concurrent event tasks. It is offline-perfect (race_interleave 358/358) but
    # HW-neutral — the lockup_TA is unchanged with it, so the two-task
    # interleave is not what fails on hardware. Held out: a lock held across an
    # HWR reset is an unknown risk, for no measured benefit.)
    if DIAG_ENTRY is None:
        a.patch(17, "ctlblk")                      # r17 = control block base, reserved for the
                                                   # whole task. A task can write its own secondary
                                                   # attributes, but only after the emit-triggered
                                                   # pds1 DMA sets sa up; this event program is a
                                                   # bare DOUTU with no such DMA, so sa is empty on
                                                   # entry (writing it here wedges init). The
                                                   # handlers therefore derive host-control / CCB
                                                   # pointers from r17.
    if not MULTI_MEM_CONTEXTS and DIAG_ENTRY is None:
        a.patch(14, "pdtable")                     # r14 = the host's PD table: read-only to the
                                                   # GPU, so its base lives in the (read-only)
                                                   # code, not in the writable control block
    if HEARTBEAT is not None:
        # a progress word for the host's watchdog, on a core whose EDM task
        # registers we can't name: +1 on every event
        a.i(f"ld.a.d r3, r17, #{HEARTBEAT} [moe]")
        a.i("wdf")
        a.i("imae r3, r3, #1, #1 [t2=u32]")
        a.i(f"st.a.d r17, #{HEARTBEAT}, r3 [moe]")
        a.i("idf")
        a.i("wdf")
    # init not done yet (ui32InitStatus == 0) -> init. host control = *(ctlblk + 1).
    a.i(f"ld.a.d r2, r17, #{SA_HOSTCTL} [moe]")
    a.i("wdf")
    a.i(f"ld.a.d r2, r2, #{HC_INIT} [moe]")
    a.i("wdf")
    a.i("test.and.eq p0, r2, r2")
    a.i("(p0) ba init")
    # otherwise dispatch on the pending event bits
    a.i(f"ldr r2, #{CR_EVENT_STATUS // 4}")
    a.i("wdf")
    # acknowledge a pending DCU-invalidate in EUR_CR_EVENT_STATUS2 first. The
    # dispatcher must read STATUS *and* STATUS2 and clear what it sees (docs
    # §2); an earlier version only dispatched STATUS bits, so the invalidate-done the
    # kernel leaves set after SysFinalise re-triggered the event program
    # forever — a storm that starved the SW_EVENT host kick and locked the
    # core (STATUS2=0x400, event re-entered 16+
    # times, the kick never serviced). Clearing a bit that is not set is a
    # no-op, so the acknowledge is unconditional and no handler reads it.
    if DCU_INVALID:                                # a core whose STATUS2 has the bit
        a.i(f"limm r3, #{DCU_INVALID}")
        a.i(f"str #{CR_EVENT_HOST_CLEAR2 // 4}, r3")
    # acknowledge the TA-finish (0x2000) and 3D-end (0x40000 pixel back-end,
    # 0x800000 ISP end-of-tile) events in EUR_CR_EVENT_STATUS the same way: the
    # dispatch() below only tests the r2 snapshot and branches, it never clears
    # the serviced bit, and neither did the handlers — so the TA-finish event
    # re-triggered ta_finished forever, starving the next host kick (kick 2's
    # render) so do_3d was never reached, and the watchdog stormed: ta_finish
    # stuck set at every lockup dump, do_ta + ta_finished markers present but
    # do_3d absent. The DDK acks these in its
    # handlers (str 0x134 = 0x2000 at TA finish, = 0x40000 at 3D end). Clearing an
    # unset bit is a no-op. Mask the acknowledge to the r2 SNAPSHOT (and r3,r2,r3)
    # so we clear ONLY the bits this task will dispatch: a bit that set after the
    # snapshot (e.g. a TA finish for a kick not yet dispatched) must survive to
    # re-trigger the event, else that kick is lost (race_interleave.py: the
    # unconditional ack dropped a kick in ~5 interleavings).
    # EVENT_TIMER (0x20000000, bit 29) is acked here too: init enables the periodic
    # timer to keep the EDM alive (see init_set), and if its event bit is not cleared
    # the event program re-triggers continuously (a storm). Acked and NOT dispatched,
    # a timer-only event falls through to `ba kick`, which returns at once on an empty
    # CCB — so each period runs one short task (0xb14 moves) and no more.
    # NOTE: 0x1 (DPM_3D_MEM_FREE) is deliberately NOT acked here -- free_done acks it.
    # A render completes via a RENDEZVOUS of two events (the DDK's trace): 0x40000
    # (pixel back-end, after the ISP clock idles) runs pixel_done and clears WAIT_3D; 0x1
    # runs free_done and clears WAIT_FREE; whichever runs SECOND does the completion. If
    # 0x1 arrives coincident with 0x40000, dispatch runs pixel_done (clears WAIT_3D, ends
    # as WAIT_FREE is still set); leaving 0x1 un-acked lets it re-raise so free_done still
    # runs and completes (else the render would never complete and a deferred cleanup
    # would stick).
    a.i(f"limm r3, #{0x2000 | 0x40000 | 0x800000 | EVENT_TIMER}")
    a.i("and r3, r2, r3")                          # ack only what the snapshot saw
    a.i(f"str #{CR_EVENT_HOST_CLEAR // 4}, r3")
    a.dispatch(0x2000, "ta_finished")              # a TA finished
    a.dispatch(0x40000, "pixel_done")              # pixel back-end: rendezvous half 1 (3D writeout)
    a.dispatch(0x800000, "pixel_done")             # ISP end of tile (same handler)
    a.dispatch(0x1, "free_done")                   # DPM 3D memory free: rendezvous half 2
    a.dispatch(0x6, "k_oom")                        # DPM out of memory (MT 0x2 | GBL 0x4): pause + terminate
    a.dispatch(0x1000, "k_oom_terminate")           # TA_TERMINATE after an OOM pause: the partial render
    # 0x6 before 0x1000 (the proven order): k_oom always acks 0x6 and queues an overlap in
    # SC_OOM_PENDING, so a 2nd OOM is deferred by flag, never left pending to re-enter the partial render.
    # A timer-only event (bit 29 set, no TA/3D bit above) must NOT scan the kernel
    # CCB: at 1 kHz a CCB scan every tick races the doorbell task's dispatch /
    # read-offset advance (the two-task RO race — frames 2+ HWR).
    # The DDK's timer path only samples AP/lockup, never the CCB. So a timer event
    # ends via kick_end, which checks idle and re-raises ACTIVE_POWER but dispatches
    # nothing. Real host kicks arrive as their own EUR_CR_EVENT_KICK doorbell events
    # (sgxutils.c SGXScheduleCCBCommand) which leave NO EVENT_STATUS bit, so they
    # fall through (no bit 29) to `kick` and are serviced there. Gating `kick` on
    # SW_EVENT (0x4000) was wrong: that bit is the microkernel->host signal, not an
    # incoming event (GETMISCINFO sat unconsumed, core looping HWR).
    a.i("limm r4, #0x20000000")
    a.i("test.and.ne p0, r2, r4")                  # timer event (bit 29)?
    if TMR_LOG or WAIT_LOG or FRAMEPROF:
        a.i("(!p0) ba not_timer_evt")              # not timer -> service doorbell
        if FRAMEPROF:
            a.i(f"ld.a.d r4, r17, #{FP_TICK} [moe]"); a.i("wdf")
            a.i("imae r4, r4, #1, #1 [t2=u32]")    # frame-profile tick counter (0x3b) += 1
            a.st_sa(SA_SELF, FP_TICK, 4)
        if TMR_LOG:
            a.i("ld.a.d r4, r17, #0x38 [moe]"); a.i("wdf")
            a.i("imae r4, r4, #1, #1 [t2=u32]")    # tick counter (0x38) += 1
            a.st_sa(SA_SELF, 0x38, 4)
        if WAIT_LOG:
            # per-tick snapshot, last wins: the hang state as of the last tick
            a.i(f"ldr r4, #{CR_EVENT_STATUS // 4}"); a.i("wdf"); a.st_sa(SA_SELF, 0x37, 4)
            a.i(f"ldr r4, #{CR_EVENT_STATUS2 // 4}"); a.i("wdf"); a.st_sa(SA_SELF, 0x38, 4)
            a.i("ldr r4, #%d" % (CR_CLKGATESTATUS // 4)); a.i("wdf"); a.st_sa(SA_SELF, 0x39, 4)
            a.i("ld.a.d r4, r17, #0x3b [moe]"); a.i("wdf")
            a.i("imae r4, r4, #1, #1 [t2=u32]"); a.st_sa(SA_SELF, 0x3b, 4)  # tick counter
        a.i("ba tmr_tick_check")                   # timer event: check for a coincident doorbell
        a.label("not_timer_evt")
    else:
        a.i("(p0) ba tmr_tick_check")              # timer event: check for a coincident doorbell
    a.i("ba kick")                                 # not a timer event (bare doorbell): service it
    # A timer-only event (bit 29) must still service a HOST KICK that coincided with
    # this tick: a doorbell leaves NO EVENT_STATUS bit, so when it lands on the same
    # pass the snapshot shows only bit 29. Ending here without looking strands the
    # command in the kernel CCB until the NEXT doorbell -- which then runs the stale
    # kick against a context being torn down and FAULTS the EDM (the WAIT_LOG freeze
    # with waitsite=0). So read the kernel CCB: a command waiting
    # (WO != RO) -> `kick` services it; empty -> end directly (NOT via kick_end, whose
    # idle ACTIVE_POWER raise would fire a host IRQ every 1 kHz tick).
    a.label("tmr_tick_check")
    # (Escape (b), the OOM terminate watchdog, was dropped: not DDK behaviour. The next-OOM grant
    # is the real scene-finish mechanism, and the teardown + 0x658 fixes removed the hang it guarded.)
    a.i(f"ld.a.d r5, r17, #{SA_KCCBCTL} [moe]")
    a.i("wdf")
    a.i("ld.a.d r6, r5, #0 [moe]")                 # kernel CCB write offset
    a.i("ld.a.d r7, r5, #1 [moe]")                 # read offset
    a.i("wdf")
    a.i("test.xor.ne p0, r6, r7")                  # WO != RO -> a command is waiting
    a.i("(p0) ba kick")                            # service the coincident doorbell
    a.label("tmr_tick_end")
    a.end()                                        # empty CCB: end, no AP raise / IRQ

    # ======================================================================
    # init
    # ======================================================================
    # Reached from the event entry with `sa` populated. Clean boot: set
    # ui32InitStatus. Recovery (HWR bit set): discard the surviving kernel CCB
    # and render-in-flight list first (docs/UKERNEL.md §5a).
    a.label("init")
    a.i(f"ld.a.d r2, r17, #{SA_HOSTCTL} [moe]")   # host control = *(ctlblk+1)
    a.i("wdf")
    a.i(f"ld.a.d r2, r2, #{HC_INTCLEAR} [moe]")   # ui32InterruptClearFlags
    a.i("wdf")
    a.i(f"test.and.ne p0, r2, #{EDM_INTERRUPT_HWR}")
    a.i("(!p0) ba init_set")                       # clean boot: skip the discard
    a.i(f"ld.a.d r1, r17, #{SA_KCCBCTL} [moe]")        # kernel CCB control
    a.i("wdf")
    a.i("ld.a.d r2, r1, #0 [moe]")                 # write offset
    a.i("wdf")
    a.i("st.a.d r1, #1, r2 [moe]")                 # read offset := write offset
    a.i("idf")
    a.i("wdf")
    a.i("limm r2, #0")                             # discard the render in flight
    a.i(f"st.a.d r17, #{SC_INFLIGHT}, r2 [moe]")
    a.i(f"st.a.d r17, #{SC_XFER}, r2 [moe]")   # and any transfer in flight
    a.i("idf")
    a.i("wdf")
    a.label("init_set")
    a.i("limm r0, #0")
    a.i(f"st.a.d r17, #{SC_TAFLIGHT}, r0 [moe]")  # TA-in-flight flag must start clear
    a.i(f"st.a.d r17, #{CLEANUP_DEFER_CT}, r0 [moe]")      # cleanup defer counter clear
    a.i(f"st.a.d r17, #{WAIT_3D}, r0 [moe]")              # rendezvous flags clear (no render yet)
    a.i(f"st.a.d r17, #{WAIT_FREE}, r0 [moe]")
    a.i(f"st.a.d r17, #{SC_OOM}, r0 [moe]")               # no OOM partial render pending
    if OVERLAP:
        a.i(f"st.a.d r17, #{TD_ENTRY}, r0 [moe]")          # no 3D in flight (overlap state clean at start)
        a.i(f"st.a.d r17, #{PENDING_3D}, r0 [moe]")        # no 3D queued
    a.i(f"st.a.d r17, #{SC_OOM_PENDING}, r0 [moe]")       # no deferred OOM queued
    a.i(f"st.a.d r17, #{OOM_SAVED_628}, r0 [moe]")        # no threshold grant outstanding
    a.i(f"st.a.d r17, #{OOM_FLAGS}, r0 [moe]")            # OOM grant flags clear (G/G2/GRANTED)
    a.i(f"st.a.d r17, #{OOM_MARK_TA}, r0 [moe]")          # no grant marks yet
    a.i(f"st.a.d r17, #{OOM_MARK_GL}, r0 [moe]")
    a.i(f"st.a.d r17, #{OOM_SAVED_624}, r0 [moe]")
    a.i(f"st.a.d r17, #{STAGE1_FLAG}, r0 [moe]")          # no stage-1 raise outstanding
    a.i(f"st.a.d r17, #{STAGE1_SAVED_628}, r0 [moe]")
    a.i(f"st.a.d r17, #{SLOT1_INIT}, r0 [moe]")           # slot 1's DPM control is whatever reset left: unset
    for r in INIT_DPM_CLEAR:                              # 0x65c, 0x630 = 0: clean DPM load-select and
        a.i(f"str #{r // 4}, r0")                         # slot select, no stale OOM-shuffle state for
                                                          # the first post-reset 3D
    a.i(f"st.a.d r17, #{PB_TA_LOADED}, r0 [moe]")         # no PB free list loaded yet: an HWR
    a.i(f"st.a.d r17, #{PB_3D_LOADED}, r0 [moe]")         # wiped the HW DPM, so force a reload
    # Enable EUR_CR_EVENT_TIMER (0xacc) = host timer period | ENABLE, as the DDK does
    # at init/RESUME. A periodic timer task keeps the EDM running
    # (EDMTaskReg0 0xb14 moves), so the host watchdog (SGXOSTimer: 30 ms of no
    # progress => HWRecoveryResetSGX) does NOT false-recover the core during the
    # quiescent TA1->terminate gap. Our purely event-driven EDM otherwise goes idle
    # between the two kicks, 0xb14 freezes, the watchdog HWRs mid-scene, SGXReset
    # wipes the TA DPM, and the terminate kicks into a wiped free-list -> lockup_ta
    # (DPMLOG: 25 pages live at TA1 finish, 0 at the terminate, with
    # no power event on 3/5 lockups). srvinit's init script writes 0xacc=0 (line 474);
    # our init runs after it (and after every HWR) and re-enables it. The timer stays
    # on through the whole active window; k_power will gate it on POWEROFF/IDLE as a
    # power follow-up. (The DDK init also writes 0x40=0x1ff + an 0x0e80 pulse — perf-
    # counter enable/reset — but those REGRESS the TA, left out.)
    a.i(f"ld.a.d r1, r17, #{SA_HOSTCTL} [moe]")
    a.i("wdf")
    a.i(f"ld.a.d r0, r1, #{HC_TIMERCLK} [moe]")   # host ui32uKernelTimerClock (period)
    a.i("wdf")
    if TIMER_DIV != 1:
        a.i(f"imae r0, r0, #{TIMER_DIV}, #0 [t2=u32]")   # UK_TIMERDIV: slow the keep-alive timer
    a.i(f"limm r1, #{EVENT_TIMER_ENABLE}")
    a.i("or r0, r0, r1")
    a.i(f"str #{CR_EVENT_TIMER // 4}, r0")        # EUR_CR_EVENT_TIMER = period | ENABLE
    if TMR_LOG:
        # H1: hostctl[6] (period) -> 0x35, 0xacc read-back -> 0x37. H2: a timer-tick
        # counter (0x38, ++ per timer-only event) and 0xb14^0xb20 (the watchdog's own
        # metric) at init -> 0x39 and at the terminate entry -> 0x3a (0xb14 raw -> 0x3b).
        a.i("limm r3, #0"); a.st_sa(SA_SELF, 0x38, 3)   # tick counter = 0
        a.i(f"ld.a.d r2, r17, #{SA_HOSTCTL} [moe]"); a.i("wdf")
        a.i(f"ld.a.d r3, r2, #{HC_TIMERCLK} [moe]"); a.i("wdf")
        a.st_sa(SA_SELF, 0x35, 3)                 # hostctl[6] (timer period)
        a.i(f"ldr r3, #{CR_EVENT_TIMER // 4}"); a.i("wdf")
        a.st_sa(SA_SELF, 0x37, 3)                 # 0xacc read-back (what the HW holds)
        a.i(f"ldr r3, #{0xb14 // 4}"); a.i("wdf")
        a.i(f"ldr r4, #{0xb20 // 4}"); a.i("wdf")
        a.i("xor r3, r3, r4")
        a.st_sa(SA_SELF, 0x39, 3)                 # 0xb14 ^ 0xb20 at init
    a.i(f"limm r0, #{UK_VERSION}")
    a.i(f"st.a.d r17, #{SC_VERSION}, r0 [moe]")   # advisory version word
    a.i(f"limm r0, #{EDM_INIT_COMPLETE}")
    a.i(f"ld.a.d r1, r17, #{SA_HOSTCTL} [moe]")   # host control = *(ctlblk+1)
    a.i("wdf")
    a.i(f"st.a.d r1, #{HC_INIT}, r0 [moe]")   # ui32InitStatus = INIT_COMPLETE
    a.i("idf")
    a.i("wdf")
    a.end()

    # ======================================================================
    # host-kick dispatch entry
    # ======================================================================
    # The event program starts this when a kernel CCB command is waiting. Read
    # the command at the read offset; its ServiceAddress is the command type
    # (our host-kick table is the identity, docs/UKERNEL.md §3). Dispatch, then
    # advance the read offset and end. One command per entry (one context).
    a.label("kick")
    # a host kick leaves a command in the kernel CCB; find it at the read
    # offset. There is no SW_EVENT to acknowledge — the host kicks via the
    # EUR_CR_EVENT_KICK doorbell, not by setting a status bit, and SW_EVENT in
    # EUR_CR_EVENT_STATUS is the microkernel->host signal the host ISR clears
    # (see the event dispatch above).
    a.i(f"ld.a.d r1, r17, #{SA_KCCBCTL} [moe]")     # kernel CCB control
    a.i("wdf")
    a.i("ld.a.d r2, r1, #0 [moe]")              # write offset (slot index)
    a.i("ld.a.d r3, r1, #1 [moe]")              # read offset (slot index)
    a.i("wdf")
    a.i("test.xor.eq p0, r2, r3")               # empty?
    a.i("(p0) ba kick_end")
    # command = kernel CCB base + read_offset * 32
    a.i(f"ld.a.d r6, r17, #{SA_KCCB} [moe]")
    a.i("wdf")
    a.i(f"imae r6, r3, #{CMD_WORDS * 4}, r6 [t2=u32]")   # r6 = &command (bytes)
    # honour ui32CacheControl (command word 1) once per command, BEFORE dispatch (as the
    # DDK does). INVAL_BIF_PD (0x2) -> BIF_CTRL = INVALDC (8), which also flushes the
    # page-table cache; else INVAL_BIF_PT (0x1) -> BIF_CTRL = FLUSH (4); INVAL_DATA (0x8)
    # -> MADD invalidate (0x804 bit 28) + wait EVENT_STATUS b26. INVAL_SL (0x4) has no SLC
    # on this core. do_ta/k_transfer pulse INVALDC only on a PD SWITCH; this honours a
    # VM_UNMAP's INVAL_BIF_PT on ANY later command, without which the BIF keeps stale PTEs
    # to freed pages -> a GPU use-after-free into recycled memory. The per-type
    # handlers never read word 1. BIF_CTRL is 0 between pulses here, so restore 0 (do_ta style).
    a.i(f"ld.a.d r4, r6, #{CMD_CACHECTL} [moe]")         # r4 = ui32CacheControl
    a.i("wdf")
    a.i("test.and.eq p0, r4, #2")                        # PD clear? -> try PT
    a.i("(p0) ba cc_pt")
    a.i("limm r2, #8")                                   # BIF_CTRL = INVALDC (dir + PT)
    a.i(f"str #{0xc00 // 4}, r2")
    a.i("limm r2, #0")
    a.i(f"str #{0xc00 // 4}, r2")
    if FIX_HW_BRN_28889:
        # BRN_28889: no data invalidate in the same pass as a BIF one. Leave the
        # command queued with only its data bit; the next event services it.
        a.i("test.and.eq p0, r4, #8")
        a.i("(p0) ba cc_data")
        a.i("limm r2, #8")
        a.i("and r4, r4, r2")
        a.i(f"st.a.d r6, #{CMD_CACHECTL}, r4 [moe]")
        a.i("idf")
        a.i("wdf")
        a.i("ba kick_end")
    a.i("ba cc_data")
    a.label("cc_pt")
    a.i("test.and.eq p0, r4, #1")                        # PT clear? -> data
    a.i("(p0) ba cc_data")
    a.i("limm r2, #4")                                   # BIF_CTRL = FLUSH (page-table)
    a.i(f"str #{0xc00 // 4}, r2")
    a.i("limm r2, #0")
    a.i(f"str #{0xc00 // 4}, r2")
    if FIX_HW_BRN_28889:
        # BRN_28889: no data invalidate in the same pass as a BIF one. Leave the
        # command queued with only its data bit; the next event services it.
        a.i("test.and.eq p0, r4, #8")
        a.i("(p0) ba cc_data")
        a.i("limm r2, #8")
        a.i("and r4, r4, r2")
        a.i(f"st.a.d r6, #{CMD_CACHECTL}, r4 [moe]")
        a.i("idf")
        a.i("wdf")
        a.i("ba kick_end")
    a.label("cc_data")
    a.i("test.and.eq p0, r4, #8")                        # DATA clear? -> dispatch
    a.i("(p0) ba cc_done")
    a.i("limm r5, #268435456")                           # 0x10000000: MADD cache invalidate
    a.i(f"ldr r4, #{0x804 // 4}")
    a.i("wdf")
    a.i("or r4, r4, r5")
    a.i(f"str #{0x804 // 4}, r4")
    a.wait(CR_EVENT_STATUS, 0x04000000, CR_EVENT_HOST_CLEAR)   # MADD done (clobbers r5, r6)
    a.i(f"ld.a.d r6, r17, #{SA_KCCB} [moe]")             # recompute &command (wait clobbered r6)
    a.i("wdf")
    a.i(f"imae r6, r3, #{CMD_WORDS * 4}, r6 [t2=u32]")
    a.label("cc_done")
    a.i(f"ld.a.d r7, r6, #{CMD_SERVICE} [moe]")          # type
    a.i(f"ld.a.d r8, r6, #{CMD_DATA1} [moe]")            # data[1] = context
    a.i("wdf")
    a.i(f"ld.a.d r9, r6, #{CMD_DATA0} [moe]")   # data[0]
    a.i("wdf")
    # dispatch on the command type (our host-kick table is the identity)
    a.i(f"test.and.eq p0, r7, r7")              # type 0 = TA
    a.i("(p0) ba k_ta")
    a.i(f"test.xor.eq p0, r7, #{CMD_GETMISCINFO}")
    a.i("(p0) ba k_getmiscinfo")
    a.i(f"test.xor.eq p0, r7, #{CMD_POWER}")
    a.i("(p0) ba k_power")
    a.i(f"test.xor.eq p0, r7, #{CMD_CTXSUSPEND}")
    a.i("(p0) ba k_ctxsuspend")
    a.i(f"test.xor.eq p0, r7, #{CMD_CLEANUP}")
    a.i("(p0) ba k_cleanup")
    a.i(f"test.xor.eq p0, r7, #{CMD_TRANSFER}")
    a.i("(p0) ba k_transfer")
    a.i("ba kick_adv")                          # process-queues / unhandled: nothing
    a.label("k_ta")
    # serialise: at most one TA in flight. If one is running, leave this kick in
    # the kernel CCB (don't advance RO) and let ta_finished re-launch it, so we
    # never re-kick a busy TA and ta_finished always acts on the in-flight command.
    a.i(f"ld.a.d r2, r17, #{SC_TAFLIGHT} [moe]")
    a.i("wdf")
    a.i("test.and.ne p0, r2, r2")               # TA already in flight -> defer
    a.i("(p0) ba k_ta_defer")
    a.i("mov r16, r8 [mask=1]")                 # context
    a.i("savl r23")
    a.i("ba do_ta [link]")
    a.i("setl r23")
    a.i("ba kick_adv")
    a.label("k_ta_defer")
    if DEBUG_SENTINEL:
        a.i("limm r6, #0x0DEF"); a.st_sa(SA_SELF, 0x3b, 6)   # saw a deferred kick
    a.i("ba kick_end")
    # get-misc-info: core identity, version and struct sizes into the misc
    # buffer (data[1]), then set READY (docs/UKERNEL.md §3 "Get misc info")
    a.label("k_getmiscinfo")
    a.i("mov r2, r8 [mask=1]")                  # misc buffer
    a.i(f"ldr r1, #{CR_CORE_REVISION // 4}")
    a.i(f"ldr r3, #{CR_CORE_ID // 4}")
    a.i("wdf")
    a.i("st.a.d r2, #1, r1 [moe]")              # core revision
    a.i("st.a.d r2, #2, r3 [moe]")              # core id
    for word, val in ((3, MISC_VER3), (4, MISC_BUILD), (5, MISC_CORE), (6, MISC_VER6), (7, MISC_BUILDOPTS)):
        a.i(f"limm r1, #{val}")
        a.i(f"st.a.d r2, #{word}, r1 [moe]")
    a.i("ld.a.d r3, r2, #0 [moe]")              # flags
    a.i("wdf")
    a.i(f"test.and.ne p0, r3, #{MISC_GET_STRUCT_SIZES}")
    a.i("(!p0) ba k_misc_ready")
    for word, val in MISC_SIZES:
        a.i(f"limm r1, #{val}")
        a.i(f"st.a.d r2, #{word}, r1 [moe]")
    a.label("k_misc_ready")
    a.i("idf")
    a.i("wdf")
    a.i("ld.a.d r3, r2, #0 [moe]")
    a.i("wdf")
    a.i(f"or r3, r3, #{MISC_READY}")
    a.i("st.a.d r2, #0, r3 [moe]")              # ui32MiscInfoFlags |= READY
    a.i("idf")
    a.i("wdf")
    a.i("ba kick_adv")
    # power (data[1] = PVRSRV_POWERCMD): report completion in ui32PowerStatus,
    # which sgxpower.c polls. The DDK also drives its power-management state
    # machine; the host-visible contract is the completion bit (full internals
    # come with the multi-render scheduler). POWEROFF -> bit 3, IDLE -> bit 2.
    a.label("k_power")
    a.i(f"ld.a.d r2, r17, #{SA_HOSTCTL} [moe]")
    a.i("wdf")
    a.i(f"ld.a.d r3, r2, #{HC_POWER} [moe]")
    a.i("wdf")
    a.i(f"test.xor.eq p0, r8, #{POWERCMD_POWEROFF}")
    a.i("(p0) or r3, r3, #8")                   # POWMAN_POWEROFF_COMPLETE
    a.i(f"test.xor.eq p0, r8, #{POWERCMD_IDLE}")
    a.i("(p0) or r3, r3, #4")                   # POWMAN_IDLE_COMPLETE
    a.i(f"st.a.d r2, #{HC_POWER}, r3 [moe]")
    a.i("idf")
    a.i("wdf")
    # Gate EUR_CR_EVENT_TIMER with the power state (DDK power-down 0x050a / RESUME
    # 0x01f9). POWEROFF/IDLE -> 0xacc=0: SGXPrePowerState stops the host timer and
    # then polls SGXPollForClockGating, which a running 1 kHz EDM timer would keep
    # the core clocked through and hang. RESUME -> re-arm (parity; this kernel's
    # resume re-runs our init via SGXInitialise, so the arm is belt-and-braces).
    # r2 = host control base, r8 = PVRSRV_POWERCMD (both from above).
    a.i(f"test.xor.eq p0, r8, #{POWERCMD_RESUME}")   # RESUME?
    a.i("(p0) ba k_power_resume")
    a.i("limm r4, #0")                               # POWEROFF / IDLE: stop the timer
    a.i(f"str #{CR_EVENT_TIMER // 4}, r4")
    a.i("ba k_power_done")
    a.label("k_power_resume")
    a.i(f"ld.a.d r4, r2, #{HC_TIMERCLK} [moe]")      # host timer period
    a.i("wdf")
    if TIMER_DIV != 1:
        a.i(f"imae r4, r4, #{TIMER_DIV}, #0 [t2=u32]")   # UK_TIMERDIV: match init's slowed timer
    a.i(f"limm r5, #{EVENT_TIMER_ENABLE}")
    a.i("or r4, r4, r5")
    a.i(f"str #{CR_EVENT_TIMER // 4}, r4")           # 0xacc = period | ENABLE
    a.label("k_power_done")
    a.i("ba kick_adv")
    # context suspend (data[0] = context, data[1] = suspend/resume): set or
    # clear the context's suspend flag (bit 4 of its flags word)
    a.label("k_ctxsuspend")
    a.i("ld.a.d r1, r9, #0 [moe]")
    a.i("wdf")
    a.i(f"test.xor.eq p0, r8, #{CTXSUSPCMD_RESUME}")
    a.i("(p0) and.inv r1, r1, #16")
    a.i("(!p0) or r1, r1, #16")
    a.i("st.a.d r9, #0, r1 [moe]")
    a.i("idf")
    a.i("wdf")
    a.i("ba kick_adv")
    # cleanup (data[0] = resource type, data[1] = resource): report completion
    # in ui32CleanupStatus, which sgxutils.c polls. The DDK also unlinks the
    # resource from the microkernel's lists only when safe; that list surgery
    # is the multi-render-bookkeeping item.
    a.label("k_cleanup")
    # Defer the cleanup while this process's render is in flight (SC_INFLIGHT): the host
    # must not free the context's PB/RT until the 3D and its page-free have finished.
    # render_done now runs on the DPM 3D-memory-free event (0x1) and clears SC_INFLIGHT
    # only there -- i.e. AFTER the pages are freed -- so SC_INFLIGHT alone covers both the
    # render and its page release. Leave the command un-acked and the kernel CCB un-
    # advanced (ba kick_end); the timer tick re-dispatches it (WO != RO), and once
    # render_done clears SC_INFLIGHT we complete. The DDK defers its ctx cleanup the same
    # way. BOUND the defer so a lost event can never stick the box: after
    # CLEANUP_DEFER_MAX ticks, force-complete + log.
    a.i(f"ld.a.d r2, r17, #{SC_INFLIGHT} [moe]")
    a.i("wdf")
    a.i("test.and.eq p0, r2, r2")              # no render in flight -> complete now
    a.i("(p0) ba k_cleanup_complete")
    a.i(f"ld.a.d r3, r17, #{CLEANUP_DEFER_CT} [moe]")
    a.i("wdf")
    a.i("imae r3, r3, #1, #1 [t2=u32]")        # defer count += 1
    a.i(f"st.a.d r17, #{CLEANUP_DEFER_CT}, r3 [moe]")
    a.i("idf")
    a.i("wdf")
    a.i(f"limm r4, #{CLEANUP_DEFER_MAX}")
    a.i("test.xor.ne p0, r3, r4")              # count != MAX -> under the bound
    a.i("(p0) ba kick_end")                    # keep deferring
    a.i("limm r4, #0x0C1E")                     # loud marker: a cleanup was force-completed
    a.st_sa(SA_SELF, 0x3c, 4)
    a.label("k_cleanup_complete")
    a.i("limm r4, #0")
    a.i(f"st.a.d r17, #{CLEANUP_DEFER_CT}, r4 [moe]")   # reset the defer counter
    # Do NOT clear the PB free-list markers here. The DDK clears them on cleanup ONLY when the
    # cleaned-up resource IS the loaded PB; an RT-dataset or render-context
    # cleanup that shares the live PB must not. Minecraft's target switches send exactly those
    # RT/ctx cleanups mid-scene; the old unconditional clear made the next kick reload the free
    # list from the frame-1 descriptor -> stale head -> r700 walks to the 0xffff terminator and
    # faults. A same-address PB realloc can't happen in one session: the
    # driver keeps PB descriptors until the next microkernel restart, and init clears the
    # markers then -- the only time a PB address is reused.
    a.i("idf")
    a.i("wdf")
    a.i(f"ld.a.d r2, r17, #{SA_HOSTCTL} [moe]")
    a.i("wdf")
    a.i(f"ld.a.d r3, r2, #{HC_CLEANUP} [moe]")
    a.i("wdf")
    a.i("or r3, r3, #1")                        # CLEANUPCMD_COMPLETE
    a.i(f"st.a.d r2, #{HC_CLEANUP}, r3 [moe]")
    a.i("idf")
    a.i("wdf")
    a.i("ba kick_adv")

    # ======================================================================
    # OOM partial render
    # ======================================================================
    a.label("k_oom")
    # DPM out of memory mid-scene (DPM_OUT_OF_MEMORY_MT 0x2 | _GBL 0x4): the TA filled the
    # parameter buffer while binning the open scene. Recover like the DDK:
    # the TA must be TERMINATED before the partial render. OTPM flush, hold the allocation
    # (0x69c=4), PAUSE the TA (0xa74 b0); the hardware then raises TA_TERMINATE (EVENT_STATUS b12),
    # which k_oom_terminate turns into the partial render. Terminate only when no render is in the
    # 3D (WAIT_3D clear); if one is, defer -- it must free first. Dormant unless the OOM fires.
    # Ack 0x6 unconditionally: an un-acked EVENT_STATUS bit STORMS the EDM (the DCU storm).
    # If an OOM episode is already in flight (SC_OOM != 0), remember it in SC_OOM_PENDING and let this
    # episode's teardown re-dispatch k_oom -- the DDK's post-partial re-check, storm-free.
    a.i("limm r4, #6")
    a.i(f"str #{CR_EVENT_HOST_CLEAR // 4}, r4")       # ack EVENT_STATUS 0x6 (MT | GBL)
    a.i(f"ld.a.d r2, r17, #{SC_OOM} [moe]")           # already handling an OOM?
    a.i("wdf")
    a.i("test.and.eq p0, r2, r2")                     # SC_OOM == 0 -> take this OOM now
    a.i("(p0) ba k_oom_take")
    a.i("limm r4, #1")
    a.i(f"st.a.d r17, #{SC_OOM_PENDING}, r4 [moe]")   # SC_OOM != 0 -> queue it; teardown re-dispatches k_oom
    a.i("ba kick_end")
    a.label("k_oom_take")
    a.i(f"ld.a.d r2, r17, #{SC_RD} [moe]")            # open scene present?
    a.i("wdf")
    a.i("test.and.eq p0, r2, r2")
    a.i("(p0) ba kick_end")                           # no open scene -> nothing to do (already acked)
    a.i(f"ld.a.d r2, r17, #{WAIT_3D} [moe]")          # a render's 3D in flight?
    a.i("wdf")
    a.i("test.and.ne p0, r2, r2")
    a.i("(p0) ba kick_end")                           # busy -> defer (serialized hits WAIT_3D clear)
    # Stage-1 gate: 0x734 b0 = macrotile (MT) shortage; moving the GLOBAL threshold can't
    # help an MT shortage, so skip stage 1 and go to the decision. Otherwise the DDK alternates a stage-1 RAISE
    # (flag clear) with its UNDO (flag set): the raise moves the global threshold to the current 0x72c count and
    # reloads the TA free list WITHOUT a render; the undo restores + reloads, then the scene gets a partial
    # render (or grant). This is the DDK's free-list-sync mechanism -- the reload rides with the threshold move,
    # so there is NO bare reload at the partial render (a bare reload leaves a seed the ring can't track).
    a.i(f"ldr r2, #{0x734 // 4}")
    a.i("wdf")
    a.i("test.and.ne p0, r2, #1")                     # 0x734 b0 (MT shortage) -> skip stage 1
    a.i("(p0) ba k_oom_decision")
    a.i(f"ld.a.d r2, r17, #{STAGE1_FLAG} [moe]")
    a.i("wdf")
    a.i("test.and.ne p0, r2, r2")                     # stage-1 flag set -> UNDO
    a.i("(p0) ba k_oom_stage1_undo")
    # STAGE-1 RAISE: 0x628 -> current count, reload, no render. Flag carries across scenes; next OOM undoes it.
    a.i(f"ldr r4, #{0x628 // 4}")                     # save the global threshold
    a.i("wdf")
    a.i(f"st.a.d r17, #{STAGE1_SAVED_628}, r4 [moe]")
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("or r5, r5, #1")                             # pause the TA
    a.i(f"str #{0xa74 // 4}, r5")
    a.ta_settle("k_s1r")
    a.i(f"ldr r4, #{0x72c // 4}")                     # 0x628 = (0x72c>>16) | 0x10000 (may lower it; don't clamp)
    a.i("wdf")
    a.i("shr r4, r4, #16")
    a.i("limm r5, #0x10000")
    a.i("or r4, r4, r5")
    a.i(f"str #{0x628 // 4}, r4")
    a.ta_freelist_reload()
    a.i(f"ldr r5, #{0xa74 // 4}")                     # unpause
    a.i("wdf")
    a.i("shr r5, r5, #1")
    a.i("shl r5, r5, #1")
    a.i(f"str #{0xa74 // 4}, r5")
    a.i("limm r4, #1")
    a.i(f"st.a.d r17, #{STAGE1_FLAG}, r4 [moe]")      # set the stage-1 flag
    a.i("limm r4, #1")
    a.i(f"str #{CR_DPM_OOM // 4}, r4")                # 0x69c = 1: resume the TA, NO render
    a.i("ba kick_end")
    a.label("k_oom_stage1_undo")
    # STAGE-1 UNDO: restore 0x628 + reload, then fall into the decision (partial render or +16 grant).
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("or r5, r5, #1")                             # pause the TA
    a.i(f"str #{0xa74 // 4}, r5")
    a.ta_settle("k_s1u")
    a.i(f"ld.a.d r4, r17, #{STAGE1_SAVED_628} [moe]") # 0x628 = saved | 0x10000
    a.i("wdf")
    a.i("limm r5, #0x10000")
    a.i("or r4, r4, r5")
    a.i(f"str #{0x628 // 4}, r4")
    a.ta_freelist_reload()
    a.i(f"ldr r5, #{0xa74 // 4}")                     # unpause
    a.i("wdf")
    a.i("shr r5, r5, #1")
    a.i("shl r5, r5, #1")
    a.i(f"str #{0xa74 // 4}, r5")
    a.i("limm r4, #0")
    a.i(f"st.a.d r17, #{STAGE1_FLAG}, r4 [moe]")      # clear the stage-1 flag
    a.label("k_oom_decision")
    # Grant-vs-partial decision: if the last partial render freed < 2 pages
    # (OOM_G set), this scene hasn't been granted yet (OOM_GRANTED clear), and the count is still above the
    # saved mark, take the GRANT path instead of another near-empty partial render -- that is what lets a
    # dense scene finish. Otherwise fall through to the terminate + partial render (the common case).
    a.i(f"ld.a.d r2, r17, #{OOM_FLAGS} [moe]")
    a.i("wdf")
    a.i(f"test.and.eq p0, r2, #{OOM_G}")              # OOM_G clear -> partial render
    a.i("(p0) ba k_oom_partial")
    a.i(f"test.and.ne p0, r2, #{OOM_GRANTED}")        # already granted this scene -> partial render
    a.i("(p0) ba k_oom_partial")
    a.i(f"test.and.ne p0, r2, #{OOM_G2}")             # OOM_G2 set -> decide on the GBL mark
    a.i("(p0) ba k_oom_dec_gl")
    a.i(f"ldr r3, #{0x724 // 4}")                     # TA case: count = 0x724 >> 16
    a.i("wdf")
    a.i("shr r3, r3, #16")
    a.i(f"ld.a.d r2, r17, #{OOM_MARK_TA} [moe]")
    a.i("wdf")
    a.i("test.isub16.pos p0, r2, r3")                # mark_ta - count >= 0 (count <= mark) -> partial
    a.i("(p0) ba k_oom_partial")
    a.i("ba k_oom_grant")
    a.label("k_oom_dec_gl")
    a.i(f"ldr r3, #{0x72c // 4}")                     # GBL case: count = 0x72c >> 16
    a.i("wdf")
    a.i("shr r3, r3, #16")
    a.i(f"ld.a.d r2, r17, #{OOM_MARK_GL} [moe]")
    a.i("wdf")
    a.i("test.isub16.pos p0, r2, r3")
    a.i("(p0) ba k_oom_partial")
    a.i("ba k_oom_grant")
    a.label("k_oom_partial")
    # Save the marks: mark_ta = (0x724>>16) - 2 (16-bit, count<2 wraps
    # harmlessly), mark_gl = max((0x72c>>16) - 2, 0). The teardown re-check and the next OOM compare to them.
    a.i(f"ldr r3, #{0x724 // 4}")
    a.i("wdf")
    a.i("shr r3, r3, #16")
    a.i("ima16.sgn r3, r3, #1, -#2")                  # count - 2
    a.i(f"st.a.d r17, #{OOM_MARK_TA}, r3 [moe]")
    a.i(f"ldr r3, #{0x72c // 4}")
    a.i("wdf")
    a.i("shr r3, r3, #16")
    a.i("test.isub16.neg p0, r3, r3, #2")             # r3 -= 2; p0 = (result < 0)
    a.i("(p0) limm r3, #0")                           # clamp at 0
    a.i(f"st.a.d r17, #{OOM_MARK_GL}, r3 [moe]")
    # Pause the TA for the terminate. NO free-list reload here: the DDK does the reload in stage 1 (the
    # threshold raise/undo above), never at the partial render (its teardown has no 0x684). A bare reload at the
    # partial render leaves a seed the ring can't track and leaks. HW raises TA_TERMINATE once
    # the TA is paused + held.
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("or r5, r5, #1")                             # pause the TA
    a.i(f"str #{0xa74 // 4}, r5")
    # OTPM flush before the terminate (0x24c = 1, wait OTPM_INV b20, ack).
    a.i("limm r4, #1")
    a.i(f"str #{CR_OTPM // 4}, r4")
    a.wait(CR_EVENT_STATUS, 0x100000, CR_EVENT_HOST_CLEAR)
    # hold the TA allocation (0x69c = 4); the TA is already paused. HW then raises TA_TERMINATE.
    a.i("limm r4, #4")
    a.i(f"str #{CR_DPM_OOM // 4}, r4")                # 0x69c = 4: hold the TA's allocation
    a.i("limm r4, #1")
    a.i(f"st.a.d r17, #{SC_OOM}, r4 [moe]")           # SC_OOM = 1: paused, awaiting TA_TERMINATE
    a.i("ba kick_end")

    a.label("k_oom_grant")
    # The grant: give the TA its reserve pages so a dense scene finishes,
    # instead of another partial render that frees almost nothing. Mark GRANTED, pause + settle, raise the
    # TA/GBL thresholds, reload the TA free list from the live counters, resume. No render; SC_OOM stays 0.
    # Undone at TA finish (0x624/0x628 restored, flags cleared).
    a.i(f"ld.a.d r2, r17, #{OOM_FLAGS} [moe]")
    a.i("wdf")
    a.i(f"or r2, r2, #{OOM_GRANTED}")
    a.i(f"st.a.d r17, #{OOM_FLAGS}, r2 [moe]")
    # pause the TA, then wait for 0x700 to settle: spin-read until two reads 400 apart match
    # (r17 is the control-block base here, so the settle uses r5/r6/r7, not the DDK's r16/r17).
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("or r5, r5, #1")                              # pause the TA
    a.i(f"str #{0xa74 // 4}, r5")
    a.label("k_grant_settle")
    a.i(f"ldr r5, #{0x700 // 4}")                     # first read
    a.i("wdf")
    a.i("limm r6, #400")
    a.label("k_grant_spin")
    a.i("test.isubu16.ne p2, r6, r6, #1")            # r6 -= 1, spin while nonzero
    a.i("(p2) ba k_grant_spin")
    a.i(f"ldr r7, #{0x700 // 4}")                     # second read
    a.i("wdf")
    a.i("test.xor.ne p2, r7, r5")                    # changed? re-read and re-spin
    a.i("(p2) ba k_grant_settle")
    # 0x624 += 32, saving the original for the TA-finish undo.
    a.i(f"ldr r4, #{0x624 // 4}")
    a.i("wdf")
    a.i(f"st.a.d r17, #{OOM_SAVED_624}, r4 [moe]")
    a.i("imae r4, r4, #1, #32 [t2=u32]")
    a.i(f"str #{0x624 // 4}, r4")
    # 0x628 = ((0x628 & 0xffff) + 16) | 0x10000, saving the original for the undo.
    a.i(f"ldr r4, #{0x628 // 4}")
    a.i("wdf")
    a.i(f"st.a.d r17, #{OOM_SAVED_628}, r4 [moe]")
    a.i("limm r5, #0xffff")
    a.i("and r4, r4, r5")
    a.i("imae r4, r4, #1, #16 [t2=u32]")
    a.i("limm r5, #0x10000")
    a.i("or r4, r4, r5")
    a.i(f"str #{0x628 // 4}, r4")
    # reload the TA free list from the live counters.
    a.i(f"ldr r4, #{0x700 // 4}")
    a.i("wdf")
    a.i(f"str #{0x61c // 4}, r4")
    a.i(f"ldr r4, #{0x710 // 4}")
    a.i("wdf")
    a.i("shl r4, r4, #16")
    a.i(f"str #{0x648 // 4}, r4")
    a.i(f"ldr r4, #{0x724 // 4}")
    a.i("wdf")
    a.i("shr r4, r4, #16")                            # 0x724 high half -> low 16
    a.i(f"ldr r5, #{0x72c // 4}")
    a.i("wdf")
    a.i("shr r5, r5, #16")
    a.i("shl r5, r5, #16")                            # 0x72c high half -> high 16
    a.i("or r4, r4, r5")
    a.i(f"str #{0x638 // 4}, r4")
    a.i("limm r5, #1")
    a.i(f"str #{CR_DPM_TA_FL // 4}, r5")
    a.wait(CR_EVENT_STATUS2, 0x1, CR_EVENT_HOST_CLEAR2)
    # resume: unpause the TA (0xa74 b0), then 0x69c = 1.
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("shr r5, r5, #1")
    a.i("shl r5, r5, #1")
    a.i(f"str #{0xa74 // 4}, r5")
    a.i("limm r4, #1")
    a.i(f"str #{CR_DPM_OOM // 4}, r4")                # 0x69c = 1
    a.i("ba kick_end")

    a.label("k_oom_terminate")
    # TA_TERMINATE (EVENT_STATUS b12 = 0x1000) after the OOM pause: the TA has stopped, so run the
    # partial render of the open scene now (do_3d on SC_RD/SC_PB, which do_ta left set). do_3d under
    # SC_OOM==2 skips the TPC flush and 0x630=0, does the DPM slot shuffle, and sets 0x658=1.
    a.i("limm r4, #4096")                             # 0x1000
    a.i(f"str #{CR_EVENT_HOST_CLEAR // 4}, r4")       # ack TA_TERMINATE
    a.i(f"ld.a.d r2, r17, #{SC_OOM} [moe]")
    a.i("wdf")
    a.i("test.xor.ne p0, r2, #1")                     # SC_OOM != 1 -> not our terminate, ignore
    a.i("(p0) ba kick_end")
    a.i("limm r4, #2")
    a.i(f"st.a.d r17, #{SC_OOM}, r4 [moe]")           # SC_OOM = 2: partial render in flight
    a.i("ba do_3d")                                   # partial render = do_3d on the open scene

    # transfer (data[1] = HW transfer context): a 3D-only render whose single
    # background object samples the source and writes the destination (texture
    # uploads and the swap blit; docs/TEX.md §3). The transfer context carries a
    # CCB at the same word offsets as a render context; its command's words 0..15
    # program the 3D. Point the BIF at the context, invalidate the 3D-side
    # caches, program the 3D from the command, and kick it. Completion comes
    # back through the 3D-end event like any render.
    a.label("k_transfer")
    # Defer while a render's 3D is in flight (SC_INFLIGHT). Starting the transfer's own 3D now
    # would re-kick the busy 3D and overwrite its ISP/PBE state, and the transfer's 0x63c=0
    # disarms the render's DPM page release mid-render -> those pages never return to the free
    # list -> the TA walks to the 0xffff terminator and faults. The DDK never starts a transfer
    # from the host kick; it queues it and the 3D scheduler starts it only when the 3D is free.
    # Leave the command in the kernel CCB (ba kick_end, no RO
    # advance); the completion path's re-dispatch and the timer tick relaunch it once SC_INFLIGHT
    # clears -- exactly as k_ta serialises on SC_TAFLIGHT. A readback transfer depends on the
    # render it follows, so deferring it until that render completes can't deadlock.
    a.i(f"ld.a.d r2, r17, #{SC_INFLIGHT} [moe]")
    a.i("wdf")
    a.i("test.and.ne p0, r2, r2")                   # a render in flight -> defer
    a.i("(p0) ba kick_end")
    a.i("mov r16, r8 [mask=1]")                     # transfer context
    a.i("ld.a.d r1, r16, #%d [moe]" % CTX_CCB)      # client CCB base
    a.i("ld.a.d r2, r16, #%d [moe]" % CTX_CCBCTL)   # CCB control
    a.i("wdf")
    a.i("ld.a.d r3, r2, #1 [moe]")                  # read offset (bytes)
    a.i("wdf")
    a.i("imae r9, r3, #1, r1 [t2=u32]")             # r9 = transfer command base
    if MULTI_MEM_CONTEXTS:
        a.i("ld.a.d r4, r16, #%d [moe]" % CTX_PD)   # context page directory
        a.i("wdf")
    else:
        a.job_pd(4, 16)                             # from the read-only PD table
    a.i(f"str #{CTX_DIR_LIST // 4}, r4")            # DIR_LIST_BASE1 = ctx PD (slot 1), list 0 (kernel PD) left intact
    # BIF directory-cache invalidate after the PD switch (EUR_CR_BIF_CTRL 0xc00 bit 3 =
    # INVALDC), as do_ta does on its PD load. The transfer path omitted it, so a 2nd
    # client's first transfer translated through the 1st client's cached list-1 PD entries
    # and BIF-faulted reading stale background/PDS program offsets (exec base +0/+0x4000
    # instead of +0x1020); only a microkernel restart cleared it. The DDK pulses this on a
    # PD change; we pulse every transfer, which is cheap.
    a.i("limm r5, #8")                              # BIF_CTRL: INVALDC (flush dir cache)
    a.i(f"str #{0xc00 // 4}, r5")
    a.i("limm r5, #0")
    a.i(f"str #{0xc00 // 4}, r5")                   # restore BIF_CTRL = 0
    if FIX_HW_BRN_28889:
        a.bif_settle()                              # the BIF invalidate drains before the MADD one
    a.invalidate(CR_PDS_INV1, INV_DONE_3D)
    # transfer's 3D field (b22-26) into 0xa74: xfer cmd word 101
    # (+0x194), masked replace like the render 3D field. r9 = transfer cmd base.
    a.i("ld.a.d r5, r9, #%d [moe]" % (0x194 // 4))   # xfer cmd+0x194 = 3D flags
    a.i(f"ldr r6, #{0xa74 // 4}")
    a.i("limm r7, #0xf83fffff")                      # ~0x07c00000: keep all but the 3D field
    a.i("wdf")
    a.i("and r6, r6, r7")
    a.i("or r5, r5, r6")
    a.i(f"str #{0xa74 // 4}, r5")
    a.program(9, T3_CMD)
    a.consts(T3_CONST)
    a.st_sa(SA_SELF, SC_XFER, 16)                   # remember the transfer in flight
    # route the transfer's 3D requestor to list 1 (the ctx PD set in 0xc38 above), as
    # do_3d does for a render -- EUR_CR_BIF_BANK0 = 0x101f (EDM=15, TA/3D=list 1). Without
    # it BANK0 stays at its 0x0f idle default (EDM only), so the 3D's client-address reads
    # fall through to list 0. Under srvinit list 0 is a shared PD that happens to map them;
    # under powervr_sgx list 0 is an empty idle PD and the transfer BIF-faults on a client
    # address (BANK0=0x0f, DIR_LIST_BASE0=idle, FAULT on 0x3e001000). We do
    # NOT write the ctx PD into list 0 -- that clobbers the kernel PD the 3D/ISP/PBE read
    # through and wedged srvinit (do_ta note); routing via BANK0 avoids that.
    if MULTI_MEM_CONTEXTS:                           # one context: every requestor is on list 0
        a.i(f"limm r5, #{0x1111f if XFER_BANK_ALL else 0x101f}")
        a.i(f"str #{0xc78 // 4}, r5")                # BIF_BANK0: TA/3D (or all DMs if XFER_BANK_ALL) -> list 1 (ctx PD)
        a.i(f"ldr r5, #{0xc78 // 4}")                # read-back + fence
        a.i("wdf")
    a.i("limm r5, #1")
    a.i(f"str #{CR_3D_KICK // 4}, r5")              # start the 3D
    a.i("ba kick_adv")
    a.label("kick_adv")
    # read offset := (read offset + 1) & 0xff
    a.i(f"ld.a.d r1, r17, #{SA_KCCBCTL} [moe]")
    a.i("wdf")
    a.i("ld.a.d r3, r1, #1 [moe]")
    a.i("wdf")
    a.i("imae r3, r3, #1, #1 [t2=u32]")
    a.i("and r3, r3, #255")
    a.i("st.a.d r1, #1, r3 [moe]")
    a.i("idf")
    a.i("wdf")
    a.label("kick_end")
    # active-power idle signal: if the kernel CCB is now empty and nothing is in
    # flight, tell the host the microkernel is idle so it can power the core
    # down. Without this the host never powers down a quiescent GPU and its
    # watchdog (SGXOSTimer, which reads 0xb14^0xb20) HWRs the halted, event-
    # driven EDM. ui32InterruptFlags is host-
    # control word 8; the host gates its power-down on ui32InterruptClearFlags,
    # so re-raising the bit is idempotent and never storms.
    a.i(f"ld.a.d r1, r17, #{SA_KCCBCTL} [moe]")
    a.i("wdf")
    a.i("ld.a.d r2, r1, #0 [moe]")                 # write offset
    a.i("ld.a.d r3, r1, #1 [moe]")                 # read offset
    a.i("wdf")
    if CCB_TRACE:
        a.st_sa(SA_SELF, 0x3a, 2)                  # kernel CCB write offset
        a.st_sa(SA_SELF, 0x3b, 3)                  # kernel CCB read offset
        a.i("idf"); a.i("wdf")
    if not NO_IDLE_AP:
        a.i("test.xor.eq p0, r2, r3")                  # queue empty?
        a.i("(!p0) ba kick_idle_done")
        a.i(f"ld.a.d r2, r17, #{SC_INFLIGHT} [moe]")   # a render in flight?
        a.i("wdf")
        a.i("test.and.eq p0, r2, r2")
        a.i("(!p0) ba kick_idle_done")
        a.i(f"ld.a.d r2, r17, #{SC_XFER} [moe]")        # a transfer in flight?
        a.i("wdf")
        a.i("test.and.eq p0, r2, r2")
        a.i("(!p0) ba kick_idle_done")
        # idle: raise ACTIVE_POWER in host control, then the host IRQ
        a.i(f"ld.a.d r1, r17, #{SA_HOSTCTL} [moe]")
        a.i("wdf")
        a.i(f"ld.a.d r2, r1, #{HC_INTFLAGS} [moe]")
        a.i("wdf")
        a.i(f"or r2, r2, #{EDM_INTERRUPT_ACTIVE_POWER}")
        a.i(f"st.a.d r1, #{HC_INTFLAGS}, r2 [moe]")
        a.i("idf")
        a.i("wdf")
        a.i(f"limm r0, #{SW_EVENT}")
        a.i("ba raise_host_irq")                        # write SW_EVENT to EVENT_STATUS
    a.label("kick_idle_done")
    a.end()

    # ======================================================================
    # do_ta: program and start the TA for the context in r16 (subroutine)
    # ======================================================================
    a.label("do_ta")
    # client CCB and its control, then the TA command base. Load it first: the
    # first-kick flag that gates the per-scene setup lives in the command.
    a.i("ld.a.d r1, r16, #%d [moe]" % CTX_CCB)      # client CCB base
    a.i("ld.a.d r2, r16, #%d [moe]" % CTX_CCBCTL)   # client CCB control
    a.i("wdf")
    a.i("ld.a.d r3, r2, #1 [moe]")                  # client read offset (bytes)
    a.i("wdf")
    a.i("imae r9, r3, #1, r1 [t2=u32]")             # r9 = command base (bytes)
    if CCB_TRACE:
        # count++ (0x35), and record THIS do_ta's client RO (0x37), cmd+0x04
        # flags (0x38), and the kernel ready flag cmd+0x58 (0x39), pre-advance.
        a.i("ld.a.d r6, r17, #0x35 [moe]"); a.i("wdf")
        a.i("imae r6, r6, #1, #1 [t2=u32]"); a.st_sa(SA_SELF, 0x35, 6)
        a.st_sa(SA_SELF, 0x37, 3)                       # client read offset (bytes)
        a.i("ld.a.d r5, r9, #1 [moe]"); a.i("wdf"); a.st_sa(SA_SELF, 0x38, 5)   # cmd+0x04
        a.i("ld.a.d r5, r9, #%d [moe]" % (0x58 // 4)); a.i("wdf")
        a.st_sa(SA_SELF, 0x39, 5)                       # cmd+0x58 (kernel ready flag)
        a.i("idf"); a.i("wdf")
    # advance the client CCB read offset past this command so the NEXT kick reads
    # its own command, not this one again. The read offset (client CCB control
    # word 1, bytes) is the microkernel's to advance — the client advances the
    # write offset. Without this, every TA kick re-read kick 1's command:
    # do_ta ran twice but both reads landed on the 0x71 first kick
    # (sentinel: count 2, flag 1), so kick 2's 0x62 terminate was never
    # seen, the TA locked up, and do_3d never ran. do_ta has already taken the
    # command's pointers; the TA reads the (separate) VDM stream, not the command,
    # so freeing the slot now is safe. (Single frame: no wrap; a multi-frame CCB
    # must mask by its size.)
    a.i("ld.a.d r4, r9, #0 [moe]")                  # command size (cmd+0x00, bytes)
    a.i("wdf")
    a.i("imae r3, r4, #1, r3 [t2=u32]")             # read offset += command size
    a.i("st.a.d r2, #1, r3 [moe]")                  # client CCB control word 1
    a.i("idf")
    a.i("wdf")
    # first-kick flag: TA command +0x04 bit 0 is bFirstKickOrResume (0x71), and
    # is clear on the render / bLastInScene kick (0x62). A scene's first kick does
    # the full per-scene setup (TA soft reset, PB descriptor, DPM free-list + state
    # load, OTPM, RT region registers, client page directory); re-running any of
    # that on a later kick re-initialises the parameter buffer / free list
    # mid-scene and HANGS the TA (kick 2: lockup_ta, do_3d
    # unreached). The DDK does only a minimal kick there (ddk-ref "TA kick 2":
    # TE/MTE, invalidate, RD, flags, kick). r13 = 1 on the first kick, 0 on a
    # render kick; each setup block below is gated on it.
    a.i("ld.a.d r13, r9, #1 [moe]")                 # cmd+0x04 (word 1)
    a.i("wdf")
    a.i("and r13, r13, #1")                         # r13 = bFirstKickOrResume
    if RT_LOG:
        # interleave detector (render-to-texture / Minecraft): a FIRST kick (r13!=0) that starts
        # a scene on a different RT entry while a render is still in flight means two scenes share
        # our single DPM context, so this kick's dpm_load clear corrupts the in-flight scene's
        # free list (the DDK uses a 2nd DPM slot). Flag it in word 0x3c.
        # Track the OPEN scene's RT in ctlblk word 0x37, set at a FIRST kick and cleared at
        # render completion. The interleave corrupts at the TA level (B's first kick runs its
        # dpm_load clear while A's scene is open but A's 3D has not started, so SC_INFLIGHT is
        # still 0 — my earlier SC_INFLIGHT check missed it). Flag 0x3a when a first kick's RT
        # differs from the already-open one. Both words are inside the devcd dump (<=0x3a).
        a.i("test.and.ne p0, r13, r13")             # first kick (r13 != 0)?
        a.i("(!p0) ba rtlog_clear")                 # terminate (r13 == 0) -> this scene closes
        a.i(f"ld.a.d r5, r9, #{CMDTA_RTENTRY} [moe]")   # this kick's RT-data entry (cmd+0x44)
        a.i("ld.a.d r6, r17, #0x37 [moe]")          # OPEN_RT: the open scene's RT (0 = none open)
        a.i("wdf")
        a.i("st.a.d r17, #0x37, r5 [moe]")          # this scene is now the open one
        a.i("idf"); a.i("wdf")
        a.i("test.and.eq p0, r6, r6")               # none open -> not an interleave
        a.i("(p0) ba rtlog_done")
        a.i("test.xor.eq p0, r5, r6")               # same RT -> not an interleave
        a.i("(p0) ba rtlog_done")
        a.i("limm r6, #0x1217")
        a.st_sa(SA_SELF, 0x3a, 6)                   # a first kick opened a 2nd scene on a diff RT
        a.i("ba rtlog_done")
        a.label("rtlog_clear")                      # terminate kick: the scene's TA closes, so a
        a.i("limm r5, #0")                          # serialized next scene isn't a false interleave
        a.i("st.a.d r17, #0x37, r5 [moe]")          # (this write is in the TA phase, not the
        a.i("idf"); a.i("wdf")                      # render-teardown the oracle diffs)
        a.label("rtlog_done")
    if WAIT_LOG:
        # stage marker: RESET to 1 only on the FIRST kick of a scene (r13!=0), so
        # 0x3a tracks the CURRENT scene's furthest stage (a terminate-kick reset
        # would mask a terminate-TA hang as "stuck at do_ta").
        a.i("test.and.eq p0, r13, r13")             # render/terminate kick? don't reset
        a.i("(p0) ba waitlog_do_ta_done")
        a.i("limm r4, #1"); a.st_sa(SA_SELF, 0x3a, 4)
        a.label("waitlog_do_ta_done")
    if DPM_LOG:
        # kick-1 entry: log 0x740 (CR read by the DDK's DPM slot-1 free step) to
        # 0x3a and STASH hostctl[11] (NumActivePowerEvents) in word 0x3c.
        # terminate entry: 0x3b = hostctl[11] now - the stashed kick-1 value
        # (>0 means a power event happened between the two kicks).
        a.i("test.and.eq p0, r13, r13")             # terminate (r13 == 0)? delta branch
        a.i("(p0) ba dpmlog_term")
        a.i(f"ldr r5, #{0x740 // 4}"); a.i("wdf"); a.st_sa(SA_SELF, 0x3a, 5)
        a.i(f"ld.a.d r5, r17, #{SA_HOSTCTL} [moe]"); a.i("wdf")
        a.i("ld.a.d r6, r5, #11 [moe]"); a.i("wdf")  # hostctl[11] NumActivePowerEvents
        a.st_sa(SA_SELF, 0x3c, 6)                    # stash kick-1 value (scratch, not dumped)
        a.i("ba dpmlog_entry_done")
        a.label("dpmlog_term")
        a.i(f"ld.a.d r5, r17, #{SA_HOSTCTL} [moe]"); a.i("wdf")
        a.i("ld.a.d r6, r5, #11 [moe]"); a.i("wdf")  # hostctl[11] at the terminate
        a.i("ld.a.d r7, r17, #0x3c [moe]"); a.i("wdf")  # the stashed kick-1 value
        a.i("test.isub16.ne p0, r6, r6, r7")         # r6 = (now - kick1) & 0xffff
        a.st_sa(SA_SELF, 0x3b, 6)
        a.label("dpmlog_entry_done")
    if TMR_LOG:
        # terminate kick only (r13==0): log 0xb14^0xb20 -> 0x3a and 0xb14 raw -> 0x3b.
        # Compared with the init value (0x39): if the watchdog metric moved between
        # init and here, the EDM ran tasks across the gap (timer works); if equal, a
        # bare timer pass doesn't advance the task registers (needs a real handler).
        a.i("test.and.ne p0, r13, r13")             # first kick (r13!=0)? skip
        a.i("(p0) ba tmrlog_term_done")
        a.i(f"ldr r5, #{0xb14 // 4}"); a.i("wdf")
        a.i(f"ldr r6, #{0xb20 // 4}"); a.i("wdf")
        a.st_sa(SA_SELF, 0x3b, 5)                   # 0xb14 raw at terminate
        a.i("xor r5, r5, r6")
        a.st_sa(SA_SELF, 0x3a, 5)                   # 0xb14 ^ 0xb20 at terminate
        a.label("tmrlog_term_done")
    if CR_PWRLOG:
        # first kick only (render kick r13==0 skips): log the power-up CR values
        # before do_ta reprograms any of them, into the dump words.
        a.i("test.and.eq p0, r13, r13")
        a.i("(p0) ba pwrlog_done")
        for word, reg in ((0x35, 0x630), (0x37, 0x080), (0x38, 0x040),
                          (0x39, 0xe80), (0x3a, 0xa74), (0x3b, 0x274)):
            a.i(f"ldr r5, #{reg // 4}")
            a.i("wdf")
            a.st_sa(SA_SELF, word, 5)
        a.i("idf")
        a.i("wdf")
        a.label("pwrlog_done")
    if DEBUG_SENTINEL:
        a.i("ld.a.d r6, r17, #0x35 [moe]"); a.i("wdf")
        a.i("imae r6, r6, #1, #1 [t2=u32]")
        a.st_sa(SA_SELF, 0x35, 6); a.st_sa(SA_SELF, 0x36, 13)
    # soft-reset the TA (EUR_CR_SOFT_RESET 0x080 bit 2) on the first kick only, so
    # it begins the scene clean (the DDK does this early, ddk-ref line 22).
    a.i("test.and.eq p0, r13, r13")                 # render kick (r13 == 0)? skip
    a.i("(p0) ba skip_ta_reset")
    a.i(f"limm r5, #{SOFT_RESET_TA}")
    a.i(f"str #{CR_SOFT_RESET // 4}, r5")
    a.i("limm r5, #0")
    a.i(f"str #{CR_SOFT_RESET // 4}, r5")
    a.label("skip_ta_reset")
    # remember context, command, and the pointers the 3D phase needs
    a.i("ld.a.d r10, r9, #%d [moe]" % CMDTA_RD)     # render details
    a.i("ld.a.d r11, r9, #%d [moe]" % CMDTA_PB)     # PB descriptor
    a.i("ld.a.d r12, r9, #%d [moe]" % CMDTA_RTENTRY)  # RT entry
    a.i("wdf")
    # Copy the HW device sync list pointer (TA command +0x3c) into render-details
    # word 56 (byte 0xE0), as the DDK does at every TA kick.
    # render_done reads rd+0xE0 to find the dependency block and sync1;
    # the client leaves rd+0xE0 zeroed (sgxgen memsets the 0x1ec render details and
    # never sets it), so without this copy the 3D-end sync-1 clear is skipped on HW.
    a.i("ld.a.d r5, r9, #%d [moe]" % (0x3c // 4))   # cmd+0x3c = HW device sync list
    a.i("wdf")
    a.i("st.a.d r10, #%d, r5 [moe]" % (0xE0 // 4))  # rd+0xE0 = sync list pointer
    a.i("idf")
    a.i("wdf")
    a.st_sa(SA_SELF, SC_CTX, 16)
    a.st_sa(SA_SELF, SC_CMD, 9)
    a.st_sa(SA_SELF, SC_RD, 10)
    a.st_sa(SA_SELF, SC_PB, 11)
    a.st_sa(SA_SELF, SC_RTENTRY, 12)
    # record this render in flight (the DDK's render-in-flight bookkeeping, so
    # the completion handler can free it and cleanup/power can tell a render is
    # running): the control-block in-flight slot holds this RT entry, the RT
    # entry points back at this render, and the RT-data completion flag is reset.
    a.st_sa(SA_SELF, SC_INFLIGHT, 12)              # control block +0x50 = RT entry
    a.i("st.a.d r12, #%d, r10 [moe]" % RT_RENDER)  # rtentry+0x4 = render details
    a.i("ld.a.d r3, r12, #%d [moe]" % RT_DATA)     # rtdata = *(rtentry+0x30)
    a.i("wdf")
    a.i("st.a.d r3, #0, #0 [moe]")                 # rtdata[0] = 0 (render in progress)
    a.i("idf")
    a.i("wdf")
    # point the BIF at this context's page directory. Load the client PD into
    # directory-list 1 (EUR_CR_BIF_DIR_LIST_BASE1 = 0xc38), leaving list 0
    # (0xc84 = the kernel PD that the 3D/ISP/PBE still read through) INTACT, pulse
    # the BIF directory-cache invalidate (EUR_CR_BIF_CTRL 0xc00 bit 3 = INVALDC),
    # then route the TA requestor to list 1 while the EDM stays on list 15
    # (EUR_CR_BIF_BANK0 = 0xc78 = 0x1f: b0-3 EDM=15, b4-7 TA=1). We used to write
    # the client PD into list 0 (0xc84), which clobbered the kernel PD and wedged
    # the 3D: it reads kernel data through list 0 and stalls (lockup_3d, BIF_FAULT
    # 0). The DDK keeps 0xc84, puts the ctx PD in 0xc38,
    # and BANK0 goes 0x0f->0x1f(TA)->0x101f(3D). client PD (0xc38): first kick
    # only — the render kick reuses the scene's PD (ddk-ref "TA kick 2" = 0x1f, no
    # reload). Then invalidate the PDS/USE caches.
    if OVERLAP:
        a.i("limm r7, #0")                          # r7 = "do INVALDC" flag (0 = render kick or PD-hit)
    a.i("test.and.eq p0, r13, r13")                 # render kick (r13 == 0)? skip
    a.i("(p0) ba skip_bif_pd")
    if OVERLAP:
        # DDK PD-point routine: skip the DIR_LIST_BASE1 write AND the INVALDC when list
        # 1 already holds this PD (a hit). On the overlap route this first kick is B's TA WHILE A's
        # 3D is translating through list 1 (same-context -> same PD -> a hit), and a directory-cache
        # invalidate under a live 3D is the overlap-only, sim-invisible hazard that refuses B's later
        # 3D kick (the DDK invalidates only on a real PD change, never evicting the list
        # the other unit uses). Serial never hits it (no 3D runs during a TA first kick), so this stays
        # OVERLAP-gated and the serial image is byte-identical.
        a.i(f"ldr r5, #{0xc38 // 4}")               # current list-1 PD
        a.i("ld.a.d r4, r16, #%d [moe]" % CTX_PD)   # this context's PD
        a.i("wdf")
        a.i("test.xor.eq p0, r5, r4")               # 0xc38 == CTX_PD -> unchanged (hit): skip write + INVALDC
        a.i("(p0) ba skip_bif_pd")
        a.i(f"str #{0xc38 // 4}, r4")               # real PD change (miss): load list 1
        a.i("limm r7, #1")                          # and do the INVALDC
    else:
        if MULTI_MEM_CONTEXTS:
            a.i("ld.a.d r4, r16, #%d [moe]" % CTX_PD)
            a.i("wdf")
        else:
            a.job_pd(4, 16)                         # from the read-only PD table
        a.i(f"str #{CTX_DIR_LIST // 4}, r4")        # BIF_DIR_LIST_BASE1 = client PD (slot 1)
    a.label("skip_bif_pd")
    if OVERLAP:
        # PRESERVE the 3D nibble (BANK0 b12-15). On a host kick arriving while a previous render's 3D is still
        # in flight (credit=2: the next command's EVENT_KICK comes during job N's 3D), a bare BANK0=0x1f would
        # ZERO b12-15 and re-route N's in-flight 3D to list 0 = the idle PD -> the 3D faults on its next fetch
        # (the storm symptom: BANK0 0x1f, 3D on the idle DIR_LIST_BASE0). Read-modify-write so the
        # 3D keeps its list: result = (BANK0 & 0xf000) | 0x1f (EDM=15, TA=1, 3D nibble untouched).
        a.i(f"ldr r5, #{0xc78 // 4}")
        a.i("wdf")
        a.i("limm r6, #0xf000")
        a.i("and r5, r5, r6")
        a.i("limm r6, #0x1f")
        a.i("or r5, r5, r6")
    elif MULTI_MEM_CONTEXTS:
        a.i("limm r5, #0x1f")                       # BIF_BANK0: EDM=list 15, TA=list 1
    if MULTI_MEM_CONTEXTS:
        a.i(f"str #{0xc78 // 4}, r5")
    # directory-cache invalidate (INVALDC): first kick only, and under OVERLAP only on a real PD
    # change (r7 set above) -- never under a live 3D on a PD hit (the queued-kick fix).
    if OVERLAP:
        a.i("test.and.eq p0, r7, r7")               # r7 == 0 (render kick or PD hit) -> skip INVALDC
    else:
        a.i("test.and.eq p0, r13, r13")             # render kick? skip INVALDC
    a.i("(p0) ba skip_bif_invaldc")
    a.i("limm r5, #8")                              # BIF_CTRL: INVALDC (flush dir cache)
    a.i(f"str #{0xc00 // 4}, r5")
    a.i("limm r5, #0")
    a.i(f"str #{0xc00 // 4}, r5")
    if FIX_HW_BRN_28889:
        a.bif_settle()                              # the BIF invalidate drains before the MADD one
    a.label("skip_bif_invaldc")
    a.invalidate(CR_PDS_INV0, INV_DONE_TA)
    # 0xa74's TA-flags field (b27-31) is written below, read-modify-write so
    # srvinit's init bits (0x00208200, b9/b15/b21 — TA/ISP config the 3D needs)
    # are PRESERVED. We used to clear 0xa74 to 0 first, which dropped 0x208200:
    # the TA still ran but the 3D stalled after the kick (TRIG_3D, lockup_3d, no
    # tile retires; DDK a74=0x50208200, ours was 0x50000000).
    # parameter-buffer registers + TA free-list load: first kick only. These set
    # up the scene's parameter buffer; re-running them on the render kick reloads
    # the free list the first kick already allocated pages from and hangs the TA
    # (the DDK skips them, ddk-ref "TA kick 2").
    a.i("test.and.eq p0, r13, r13")             # render kick? skip PB + free list
    a.i("(p0) ba skip_pb_fl")
    # Load the TA free list only when the PB changed. The DDK loads it ONCE PER PB and
    # lets the hardware keep the free-list ring in sync; we reloaded on every scene's
    # first kick, which re-seeds the head from pb+0x18 (last frame's saved 0x700) and
    # skips the ~25 pages the 3D freed, exhausting the PB at ~frame 40 (DPM_OUT_OF_MEM_MT,
    # the M1 exit). PB_TA_LOADED is the DDK's own
    # ta3d+0xb8 marker; init and cleanup reset it so an HWR or a same-address realloc reloads.
    a.i(f"ld.a.d r5, r17, #{PB_TA_LOADED} [moe]")
    a.i("wdf")
    a.i("test.xor.eq p0, r5, r11")              # same PB already loaded -> skip the reload
    a.i("(p0) ba skip_pb_fl")
    a.program(11, TA_PB)
    a.dpm_freelist(3, CR_DPM_TA_FL, 0x1)        # load the TA free list before the kick
    a.i(f"st.a.d r17, #{PB_TA_LOADED}, r11 [moe]")   # remember the loaded PB
    a.i("idf")
    a.i("wdf")
    if DPM_LOG:                                 # point A: 0x700 after the free-list load (kick 1)
        a.i(f"ldr r5, #{0x700 // 4}"); a.i("wdf"); a.st_sa(SA_SELF, 0x35, 5)
    a.label("skip_pb_fl")
    # TE / macrotile / screen layout from the command tail
    a.program(9, TA_TE)
    # 0xa74's TA field (b27-31) = the command's flags word (cmd+0x14), as the DDK
    # writes it after the TE registers and before the kick (ddk-ref;
    # STATE.md cmd+0x14 = 0x50000000 when kick flag 0x2000000 is clear). Masked
    # REPLACE: clear the TA field (mask ~0xf8000000) and OR the flags in, so
    # srvinit's init bits (0x208200) survive AND a stale TA field from a previous
    # scene is cleared, not accumulated. The DDK's a74 after the TA = 0x50208200.
    a.i(f"ld.a.d r9, r17, #{SC_CMD} [moe]")
    a.i("wdf")
    a.i("ld.a.d r5, r9, #5 [moe]")                   # cmd+0x14 = TA flags (b27-31)
    a.i(f"ldr r6, #{0xa74 // 4}")                    # current a74 (srvinit init 0x208200)
    a.i("limm r7, #0x07ffffff")                      # ~0xf8000000: keep all but the TA field
    a.i("wdf")
    a.i("and r6, r6, r7")
    a.i("or r5, r5, r6")                             # a74 = (a74 & ~TA-field) | flags
    a.i(f"str #{0xa74 // 4}, r5")
    # RT region-header / tail-pointer base registers (0x62c/0x634/0x234): first
    # kick only — they describe the scene's parameter-buffer regions, set once and
    # reused (the DDK skips them on the render kick).
    a.i("test.and.eq p0, r13, r13")             # render kick? skip RT region regs
    a.i("(p0) ba skip_rt")
    if OVERLAP:
        # Slot pick, as the DDK does: bind the DPM slot the in-flight 3D is NOT on, so this TA can bin while
        # that 3D renders + frees the other slot. 0x630 b2 = TA slot, b1 = 3D slot, b0 = first-kick slot.
        # A 3D is in flight iff TD_ENTRY != 0; its slot is 0x630 b1. Only slot 1 is needed (one 3D at a time):
        # a 3D on slot 0 -> this TA on slot 1 (0x630 = 4 then 5, RT state reg 0x674); else slot 0 (0x630 = 0,
        # RT state reg 0x62c via TA_RT). Matches interleave_ours.py --order overlap: 0/0/0/4,5/5/7.
        a.i(f"ld.a.d r5, r17, #{TD_ENTRY} [moe]")   # the in-flight 3D's RT entry (0 = none)
        a.i("wdf")
        a.i("test.and.eq p0, r5, r5")               # no 3D in flight -> slot 0
        a.i("(p0) ba ov_slot0")
        a.i(f"ldr r6, #{0x630 // 4}")
        a.i("wdf")
        a.i("test.and.ne p0, r6, #2")               # 3D on slot 1 (b1 set) -> this TA on slot 0
        a.i("(p0) ba ov_slot0")
        a.i("limm r5, #4")                          # slot 1: 0x630 = 4 (b2), then 5 (b2 | b0)
        a.i(f"str #{0x630 // 4}, r5")
        a.i("limm r5, #5")
        a.i(f"str #{0x630 // 4}, r5")
        a.i("ld.a.d r5, r12, #5 [moe]")             # RT entry words 5/6/7
        a.i("ld.a.d r6, r12, #6 [moe]")
        a.i("ld.a.d r7, r12, #7 [moe]")
        a.i("wdf")
        a.i(f"str #{0x674 // 4}, r5")                # slot-1 state reg = w5 (0x62c is slot 0's)
        a.i(f"str #{0x634 // 4}, r6")                # control reg = w6 (shared)
        a.i(f"str #{0x234 // 4}, r7")                # tail-pointer reg = w7 (shared)
        a.i("ba skip_rt")
        a.label("ov_slot0")
        a.i("limm r5, #0")                          # slot 0: 0x630 = 0 twice (b2 then b0; DDK writes both)
        a.i(f"str #{0x630 // 4}, r5")
        a.i(f"str #{0x630 // 4}, r5")
    a.program(12, TA_RT)
    a.label("skip_rt")
    a.program(10, TA_RD)
    # cmd -> render-details copy block (DDK routine, called on EVERY
    # TA kick; ours omitted it entirely). The render details are
    # microkernel-internal (HW never reads rd); these words feed do_3d's register
    # programming and the completion path, so without them do_3d programs the 3D from
    # stale/zero fields and the ISP stalls at tile 0. r9 = command, r10 = render
    # details. Fixed single-word copies first, batched to share fences:
    CMD2RD = [(0x1c, 0xd4), (0x3c, 0xe0), (0x74, 0xe4), (0x78, 0xe8), (0x7c, 0xec),
              (0x80, 0xf0), (0x4c, 0x118), (0x50, 0x11c), (0x48, 0x120), (0x08, 0x130),
              (0x0c, 0x134), (0x10, 0x138), (0x20, 0x140), (0x60, 0x144), (0x84, 0x168)]
    for i in range(0, len(CMD2RD), 6):
        batch = CMD2RD[i:i + 6]
        for k, (src, _) in enumerate(batch):
            a.i("ld.a.d r%d, r9, #%d [moe]" % (2 + k, src // 4))
        a.i("wdf")
        for k, (_, dst) in enumerate(batch):
            a.i("st.a.d r10, #%d, r%d [moe]" % (dst // 4, 2 + k))
        a.i("idf")
    a.i("wdf")
    # conditional: if the 3D flags (cmd+0x1c) have bit 9 (0x200), copy cmd+0x110/0x114
    # into rd+0xfc/0x100 (the DDK's 0x200-gated pair).
    a.i("ld.a.d r2, r9, #%d [moe]" % (0x1c // 4))
    a.i("wdf")
    a.i("limm r3, #512")                         # 0x200
    a.i("test.and.eq p0, r2, r3")                # (cmd+0x1c & 0x200) == 0 -> skip
    a.i("(p0) ba skip_rd_cond")
    a.i("ld.a.d r2, r9, #%d [moe]" % (0x110 // 4))
    a.i("ld.a.d r3, r9, #%d [moe]" % (0x114 // 4))
    a.i("wdf")
    a.i("st.a.d r10, #%d, r2 [moe]" % (0xfc // 4))
    a.i("st.a.d r10, #%d, r3 [moe]" % (0x100 // 4))
    a.i("idf")
    a.i("wdf")
    a.label("skip_rd_cond")
    # (The DDK also runs three counted blocks here — rd+0x168 from cmd+0x88,
    # rd+0x144 pairs from cmd+0x218, and deferred updates from cmd+0x2a8 — but they
    # feed the completion path, not do_3d's register programming (D3_RD reads only
    # rd 0x00-0xcc + 0x108), and block B's source is still being confirmed.
    # Left out until validated, so the test binary carries only the
    # confirmed fixed copies.)
    # second cache invalidate (both kicks), then the DPM state/control load. The
    # load is first kick only: it loads the scene's DPM state, and re-running it
    # mid-scene disturbs the in-flight parameter state (the DDK skips it on the
    # render kick). Done before the TE registers, dpm_load's 0x240 done event
    # never fired and do_ta stalled, so it stays
    # after them (ddk-ref-frame-registers.txt lines 106-131).
    a.invalidate(CR_PDS_INV0, INV_DONE_TA)
    a.i("test.and.eq p0, r13, r13")             # render kick? skip DPM state load
    a.i("(p0) ba skip_dpm_load")
    a.dpm_load()
    if DPM_LOG:                                 # point B: 0x700 after dpm_load (kick 1)
        a.i(f"ldr r5, #{0x700 // 4}"); a.i("wdf"); a.st_sa(SA_SELF, 0x37, 5)
    a.label("skip_dpm_load")
    # TA flags 2 (0x218): the command word, always with bit 24, plus bit 23
    # when the RT entry's word 13 bit 23 is set (docs/UKERNEL.md §7)
    a.i(f"ldr r4, #{0x218 // 4}")
    a.i("ld.a.d r3, r12, #13 [moe]")
    a.i("wdf")
    a.i("limm r5, #16777216")                   # 0x01000000, always
    a.i("or r4, r4, r5")
    a.i("test.shl.neg p0, r3, #23")
    a.i("limm r5, #8388608")                     # 0x00800000, conditional
    a.i("(p0) or r4, r4, r5")
    a.i(f"str #{0x218 // 4}, r4")
    if K2_LOG:
        # terminate kick only (r13==0): log TA1/finish's leftover state BEFORE the
        # 0x200. 0x118 EVENT_STATUS2 is the key one (already TRIG_TA/OOM?).
        a.i("test.and.ne p0, r13, r13")             # first kick (r13!=0)? skip
        a.i("(p0) ba k2log_done")
        for word, reg in ((0x35, 0x118), (0x37, 0x12c), (0x38, 0x008),
                          (0x39, 0x724), (0x3a, 0x72c), (0x3b, 0x700)):
            a.i(f"ldr r5, #{reg // 4}")
            a.i("wdf")
            a.st_sa(SA_SELF, word, 5)
        a.i("idf")
        a.i("wdf")
        a.label("k2log_done")
    # start the TA. Mark it in flight first (SC_TAFLIGHT) so a host kick arriving
    # while it runs is deferred, not dispatched on top of it.
    a.i("limm r5, #1")
    a.st_sa(SA_SELF, SC_TAFLIGHT, 5)            # TA in flight
    a.i(f"str #{CR_TA_KICK // 4}, r5")
    if FRAMEPROF:
        a.i(f"ld.a.d r4, r17, #{FP_TICK} [moe]"); a.i("wdf"); a.st_sa(SA_SELF, FP_TA_KICK, 4)  # frameprof: TA kick
    if DEBUG_SENTINEL:
        a.i("limm r6, #0xC01C"); a.st_sa(SA_SELF, 0x37, 6)
    a.i("lapc")

    # ======================================================================
    # ta_finished: write the TA status values, then program and start the 3D
    # ======================================================================
    a.label("ta_finished")
    if FRAMEPROF:
        a.i(f"ld.a.d r4, r17, #{FP_TICK} [moe]"); a.i("wdf"); a.st_sa(SA_SELF, FP_TA_FIN, 4)  # frameprof: TA finished
        a.i(f"ld.a.d r5, r17, #{FP_TA_KICK} [moe]"); a.i("wdf")
        a.i("test.isub16.ne p2, r4, r4, r5")          # r4 = (tick - TA_kick) & 0xffff = this TA's busy ticks
        a.i(f"ld.a.d r6, r17, #{FP_TA_BUSY} [moe]"); a.i("wdf")
        # imae truncates its MULTIPLICANDS to 16 bits (emu op_imae), so the running total must be the
        # full-32-bit ADDEND (ops[3], t2=u32): total = delta*1 + total, delta (<65536) in the mul slot.
        a.i("imae r6, r4, #1, r6 [t2=u32]"); a.i(f"st.a.d r17, #{FP_TA_BUSY}, r6 [moe]")   # TA-busy total += delta
        a.i(f"ld.a.d r6, r17, #{FP_COUNTS} [moe]"); a.i("wdf")
        a.i("limm r5, #1")
        a.i("imae r6, r5, #1, r6 [t2=u32]"); a.i(f"st.a.d r17, #{FP_COUNTS}, r6 [moe]")    # TA count (low 16) += 1
    if WAIT_LOG:
        a.i("limm r4, #2"); a.st_sa(SA_SELF, 0x3a, 4)   # stage: reached ta_finished
    if DEBUG_SENTINEL:
        a.i("limm r6, #0x7AF1"); a.st_sa(SA_SELF, 0x38, 6)
    # NOTE: no abort-on-TA_FINISHED here. The DDK never unpauses / writes 0x69c=1 in its TA-finish
    # handler (0 of 1206 TA_FINISHED rows land during an OOM pause). If HW does deliver a
    # 0x2000 while SC_OOM==1, the hold stays and render_complete releases it AFTER the scene's final
    # render frees the pages -- releasing earlier over-allocates past the limit (the ep#1 fault).
    a.label("ta_oom_unwound")
    # Undo a +16 grant at TA finish (the rare granted scene): restore 0x624/
    # 0x628, re-seed the TA free list, and clear the grant flags. The per-OOM free-list reload that keeps the
    # ring in sync is now in k_oom at each OOM pause (the DDK's position), NOT here -- a TA-finish reload seeds
    # a stale mid-scene snapshot and made the leak worse.
    a.i(f"ld.a.d r4, r17, #{OOM_FLAGS} [moe]")
    a.i("wdf")
    a.i(f"test.and.eq p0, r4, #{OOM_GRANTED}")        # not granted this scene -> nothing to undo
    a.i("(p0) ba ta_grant_restored")
    a.i(f"ld.a.d r4, r17, #{OOM_SAVED_628} [moe]")    # 0x628 = saved | 0x10000
    a.i("wdf")
    a.i("limm r5, #0x10000")
    a.i("or r4, r4, r5")
    a.i(f"str #{0x628 // 4}, r4")
    a.i(f"ld.a.d r4, r17, #{OOM_SAVED_624} [moe]")    # 0x624 = saved
    a.i("wdf")
    a.i(f"str #{0x624 // 4}, r4")
    # pause + settle + the reload + unpause (the DDK grant undo).
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("or r5, r5, #1")                             # pause
    a.i(f"str #{0xa74 // 4}, r5")
    a.label("ta_oom_settle")
    a.i(f"ldr r5, #{0x700 // 4}")
    a.i("wdf")
    a.i("limm r6, #400")
    a.label("ta_oom_spin")
    a.i("test.isubu16.ne p2, r6, r6, #1")
    a.i("(p2) ba ta_oom_spin")
    a.i(f"ldr r7, #{0x700 // 4}")
    a.i("wdf")
    a.i("test.xor.ne p2, r7, r5")
    a.i("(p2) ba ta_oom_settle")
    a.i(f"ldr r4, #{0x700 // 4}")
    a.i("wdf")
    a.i(f"str #{0x61c // 4}, r4")
    a.i(f"ldr r4, #{0x710 // 4}")
    a.i("wdf")
    a.i("shl r4, r4, #16")
    a.i(f"str #{0x648 // 4}, r4")
    a.i(f"ldr r4, #{0x724 // 4}")
    a.i("wdf")
    a.i("shr r4, r4, #16")
    a.i(f"ldr r5, #{0x72c // 4}")
    a.i("wdf")
    a.i("shr r5, r5, #16")
    a.i("shl r5, r5, #16")
    a.i("or r4, r4, r5")
    a.i(f"str #{0x638 // 4}, r4")
    a.i("limm r5, #1")
    a.i(f"str #{CR_DPM_TA_FL // 4}, r5")
    a.wait(CR_EVENT_STATUS2, 0x1, CR_EVENT_HOST_CLEAR2)
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("shr r5, r5, #1")
    a.i("shl r5, r5, #1")
    a.i(f"str #{0xa74 // 4}, r5")
    a.i("limm r4, #0")
    a.i(f"st.a.d r17, #{OOM_FLAGS}, r4 [moe]")
    a.i(f"st.a.d r17, #{OOM_SAVED_628}, r4 [moe]")
    a.i(f"st.a.d r17, #{OOM_SAVED_624}, r4 [moe]")
    a.label("ta_grant_restored")
    # this TA is no longer in flight; clear the flag so a deferred kick can launch.
    a.i("limm r4, #0")
    a.st_sa(SA_SELF, SC_TAFLIGHT, 4)
    # clear 0xa74's low nibble (b0-3) at TA finish, as the DDK does: per-kick TA
    # state that must not carry into the next scene (multi-frame hygiene).
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("limm r6, #0xfffffff0")
    a.i("wdf")
    a.i("and r5, r5, r6")
    a.i(f"str #{0xa74 // 4}, r5")
    # save the TA state register 0x274 across the scene's kicks. At TA finish the
    # DDK reads CR 0x274 and stores it to rd+0x124; the next kick's
    # TA_RD restore (0x274 <- rd+0x124, program() at the TA kick) reloads it. Ours
    # restored but never saved, so kick 2 got the client's 0 instead of kick 1's
    # TA state (0x00020000) -> inconsistent kick-2 tiling / region headers ->
    # the ISP stalls at tile 0 with a clean BIF. The
    # DDK does this on the non-last finish; it is harmless on the last.
    a.i(f"ld.a.d r3, r17, #{SC_RD} [moe]")            # render details
    a.i(f"ldr r4, #{0x274 // 4}")                     # CR 0x274 (TA scene state)
    a.i("wdf")
    a.i("st.a.d r3, #%d, r4 [moe]" % (0x124 // 4))    # rd+0x124 = CR 0x274
    a.i("idf")
    a.i("wdf")
    # TA status-value list (docs/UKERNEL.md §5): CMDTA_SHARED holds a count at
    # command+0x5c and that many {sStatusDevAddr, ui32StatusValue} pairs from
    # command+0x118; write each value to its address (sync-heap counters the
    # host and the next client wait on).
    a.i(f"ld.a.d r1, r17, #{SC_CMD} [moe]")   # TA command base
    a.i("wdf")
    a.i("ld.a.d r2, r1, #%d [moe]" % (0x5c // 4))     # NumTAStatusVals
    a.i("wdf")
    a.i("test.and.eq p0, r2, r2")                     # count == 0 -> skip
    a.i("(p0) ba ta_status_done")
    # entries base = command + 0x118; walk by index i (imae truncates its
    # first operand to 16 bits, so a small index must drive the addressing,
    # not a 32-bit pointer advanced through imae)
    a.i("limm r7, #%d" % 0x118)
    a.i("imae r6, r7, #1, r1 [t2=u32]")               # r6 = command + 0x118
    a.i("limm r8, #0")                                # i = 0
    a.label("ta_status_loop")
    a.i("imae r3, r8, #8, r6 [t2=u32]")               # r3 = r6 + i*8
    a.i("ld.a.d r4, r3, #0 [moe]")                    # sStatusDevAddr
    a.i("ld.a.d r5, r3, #1 [moe]")                    # ui32StatusValue
    a.i("wdf")
    a.i("st.a.d r4, #0, r5 [moe]")                    # *addr = value
    a.i("idf")
    a.i("imae r8, r8, #1, #1 [t2=u32]")               # i++
    a.i("test.isub16.ne p0, r2, r2, #1")              # --count != 0 -> loop
    a.i("(p0) ba ta_status_loop")
    a.label("ta_status_done")
    a.i("wdf")
    # start the 3D once the scene's LAST TA has finished. The last-in-scene kick
    # is the TA command's flags word (cmd+0x04) bit 1: sgxgen writes 0x62 for the
    # bLastInScene / render kick and 0x71 for a first/stream kick (bit 1 set only
    # on 0x62). Command word 0 is the command SIZE (0x2c0, STATE.md §1), not a
    # flags word — testing its bit 0 is always 0, so do_3d never ran on hardware
    # (ta_finished reached, do_3d marker absent). run_ours injects
    # the 3D phase, so it could not catch this.
    a.i(f"ld.a.d r1, r17, #{SC_CMD} [moe]")   # TA command base
    a.i("wdf")
    a.i("ld.a.d r2, r1, #1 [moe]")                    # cmd+0x04 = TA flags (0x71/0x62)
    a.i("wdf")
    a.i("test.and.ne p0, r2, #2")                     # bLastInScene (bit 1)? render
    if OVERLAP:
        # 3D QUEUE: a scene's 3D can start only when the 3D engine is free (never two 3Ds). If a 3D is in
        # flight (TD_ENTRY != 0), queue this scene's 3D; render_complete starts it after the current 3D frees.
        # Credit = 2 blocks a 3rd TA, so SC_* stays this scene's until then. Not last -> kick re-dispatch.
        a.i("(!p0) ba ov_ta_notlast")
        # TPC flush at the LAST TA finish (DDK order): flush the tail-pointer
        # cache now, while the TE/TPC still holds THIS scene's tail state and -- on the queued
        # route -- A's 3D is still running (the DDK flushes here too, mid-A-3D; safe, the TPC
        # holds the TA's tail, separate from A's active ISP). do_3d used to flush itself, which
        # on the queued route runs long after this TA finished -> B's tail state is gone and the
        # 0x428 kick is refused. Do it for BOTH the start-now and the queued paths; do_3d skips
        # it under OVERLAP. (0x224=0x40000000, wait EVENT_STATUS b10=0x400, ack 0x134=0x400.)
        a.i("limm r5, #0x40000000")
        a.i(f"str #{CR_TPC // 4}, r5")
        a.wait(CR_EVENT_STATUS, 0x400, CR_EVENT_HOST_CLEAR)
        a.i(f"ld.a.d r3, r17, #{TD_ENTRY} [moe]")      # a 3D already in flight?
        a.i("wdf")
        a.i("test.and.eq p0, r3, r3")                  # none -> start this 3D now
        a.i("(p0) ba do_3d")
        a.i("limm r3, #1")
        a.i(f"st.a.d r17, #{PENDING_3D}, r3 [moe]")     # queue it (render_complete starts it)
        a.i("ba kick")
        a.label("ov_ta_notlast")
    else:
        a.i("(p0) ba do_3d")
    if DPM_LOG:                                       # point C: TA1's finish (non-last)
        a.i(f"ldr r5, #{0x700 // 4}"); a.i("wdf"); a.st_sa(SA_SELF, 0x38, 5)
        a.i(f"ldr r5, #{0x724 // 4}"); a.i("wdf"); a.st_sa(SA_SELF, 0x39, 5)
    # not the last TA of the scene: the 3D waits. Re-enter the kick dispatch so a
    # host kick that was deferred while this TA ran (SC_TAFLIGHT) launches now (the
    # DDK re-launches the kick task from its TA-finished handler). If none is
    # pending the dispatch falls through to kick_end and the event ends.
    # NOTE: the DDK runs op0x100 #3 (USE/EDM data-cache flush) in the shared task-end
    # tail here. Tried it: no improvement (10/30 frames,
    # same as op0xff) — the cache ops are not the terminate-kick cause.
    a.i("ba kick")

    # ======================================================================
    # do_3d: program and start the 3D for the scene (run on the last TA's finish)
    # ======================================================================
    a.label("do_3d")
    if WAIT_LOG:
        a.i("limm r4, #3"); a.st_sa(SA_SELF, 0x3a, 4)   # stage: reached do_3d
    if DEBUG_SENTINEL:
        a.i("limm r6, #0x3DED"); a.st_sa(SA_SELF, 0x39, 6)
    # CR 0x630: a double-buffered TA/3D context select. The DDK r-m-w's
    # it on the TA and 3D paths so bits 0-2 are clear by the 3D kick; ours never wrote
    # it, so a non-zero power-up/srvinit value can point the 3D at the slot the TA did
    # NOT fill -> ISP stalls at tile 0 with a clean BIF. Clear bits 0-2 (the DDK's net
    # effect for this single-RT frame). Log the inherited value first (word 0x35).
    # OOM partial render (SC_OOM==2): skip the 0x630 clear AND the TPC flush below. The DPM
    # shuffle sets 0x630 itself, and the TA was already TERMINATED (TA_TERMINATE) so there is
    # no unterminated control stream to flush -- flushing it would be the normal-path assumption.
    a.i(f"ld.a.d r4, r17, #{SC_OOM} [moe]")
    a.i("wdf")
    a.i("test.xor.eq p0, r4, #2")                     # SC_OOM == 2 -> skip 0x630 clear + TPC flush
    a.i("(p0) ba do_3d_after_tpc")
    a.i(f"ldr r5, #{0x630 // 4}")
    a.i("wdf")
    if DEBUG_SENTINEL:
        a.st_sa(SA_SELF, 0x35, 5)                     # log 0x630's inherited value
    if OVERLAP:
        # 3D start: the 3D takes this scene's slot (0x630 b2) -> set b1 = b2 (DDK: 5 -> 7 for slot 1, 0 -> 0
        # for slot 0). DON'T clear the slot bits; the TA may already be binning the next scene on the other
        # slot. Then populate the 3D set so render_complete and the next TA's slot pick see this 3D in flight.
        a.i("limm r6, #4")
        a.i("and r6, r5, r6")                        # b2 = the scene's slot (0 or 4)
        a.i("shr r6, r6, #1")                        # -> b1 position (0 or 2)
        a.i("or r5, r5, r6")                         # 0x630 b1 = b2
        a.i(f"str #{0x630 // 4}, r5")
        a.i(f"ld.a.d r6, r17, #{SC_RTENTRY} [moe]")
        a.i(f"ld.a.d r7, r17, #{SC_RD} [moe]")
        a.i("wdf")
        a.i(f"st.a.d r17, #{TD_ENTRY}, r6 [moe]")    # the in-flight 3D's RT entry
        a.i(f"st.a.d r17, #{TD_RD}, r7 [moe]")       # and its render details
        a.i(f"ld.a.d r6, r17, #{SC_CTX} [moe]")
        a.i("wdf")
        a.i(f"st.a.d r17, #{TD_CTX}, r6 [moe]")      # and its HW render context
        # EARLY BIF dir-cache invalidate for the queued render (DDK, right after
        # 0x630 -> 7 and BEFORE the 3D register block): render A changed the BIF directory state,
        # so flush the MADD+USE caches and re-assert BANK0 to list 1 now, else B's 3D translates
        # through A's STALE directory cache and the 0x428 kick is REFUSED. The register VALUES
        # already match the DDK (oracle-clean); this early invalidate -- a HW effect the sim
        # can't model -- is the one structural difference left on the queued route.
        a.i("limm r5, #268435456")                     # 0x10000000: MADD cache
        a.i(f"ldr r4, #{0x804 // 4}")
        a.i("wdf")
        a.i("or r4, r4, r5")
        a.i(f"str #{0x804 // 4}, r4")
        a.wait(CR_EVENT_STATUS, 0x04000000, CR_EVENT_HOST_CLEAR)   # MADD done (b26), ack 0x134
        a.i("limm r5, #1")
        a.i(f"str #{CR_USE_INVAL // 4}, r5")           # 0xa08 USE invalidate
        a.i("limm r5, #0x101f")
        a.i(f"str #{0xc78 // 4}, r5")                  # BANK0 = 0x101f (3D requestor -> list 1)
        a.i(f"ldr r5, #{0xc78 // 4}")                  # read-back fence
        a.i("wdf")
    else:
        a.i("shr r5, r5, #3")
        a.i("shl r5, r5, #3")                         # clear bits 0-2
        a.i(f"str #{0x630 // 4}, r5")
    # flush the tail-pointer cache before the 3D. The last TA's final control-
    # stream terminates and tail pointers sit in the TPC, not yet written back to
    # the parameter buffer; without the flush the ISP walks UNTERMINATED region
    # streams through valid PB pages and hangs with BIF_FAULT=0
    # (lockup_3d, do_3d reached, render_done never). The DDK flushes it
    # first thing on the last-in-scene TA finish (ddk-ref "TA finished (2)"):
    # CR_TPC (0x224) = 0x40000000, wait EUR_CR_EVENT_STATUS bit 10 (0x400,
    # TPC_FLUSH), clear it.
    if not OVERLAP:
        # Serial flushes the TPC here. Under OVERLAP the flush moved to ta_finished's last-kick
        # path (DDK order) so the queued 3D doesn't re-flush long after its TA finished, when the
        # TE/TPC no longer holds its tail state -- which refused the 0x428 kick.
        a.i("limm r5, #0x40000000")
        a.i(f"str #{CR_TPC // 4}, r5")
        a.wait(CR_EVENT_STATUS, 0x400, CR_EVENT_HOST_CLEAR)
    a.label("do_3d_after_tpc")
    a.i(f"ld.a.d r10, r17, #{SC_RD} [moe]")   # render details
    a.i(f"ld.a.d r11, r17, #{SC_PB} [moe]")   # PB descriptor
    a.i("wdf")
    # mark the context's in-flight render (the DDK sets these once the scene's
    # last TA has finished and the 3D is about to start; the completion handler
    # clears them). No rd+0x2a4 "3D running" flag: it is never read, the DDK
    # never writes it, and at 0x2a4 it lands 0xb8 past the end of the 0x1ec-byte
    # render details, clobbering a word of the following kdata the 3D reads
    # (the sole write ours did that the DDK did not).
    a.i(f"ld.a.d r2, r17, #{SC_CTX} [moe]")
    a.i("wdf")
    a.i("st.a.d r2, #%d, r10 [moe]" % CTX_INFLIGHT0)  # ctx+0x28 = render details
    a.i("st.a.d r2, #%d, r10 [moe]" % CTX_INFLIGHT1)  # ctx+0x2c = render details
    a.i("idf")
    a.i("wdf")
    # DPM allocated-page handover to the 3D (from the DDK
    # microkernel save_ta_state + load_3d_state). The TA's
    # allocated-page counters (EUR_CR 0x700/0x710/0x724/0x72c) go through the PB
    # descriptor into the 3D's free/dealloc registers (0x604/0x64c/0x660), so the
    # 3D frees the right pages; do_3d wrote only 0x600 before.
    # Load the 3D free list + save_ta_state only when the PB changed (the per-PB rule, same
    # as the TA side: the DDK loads once per PB and the HW keeps the ring in sync). When it
    # is unchanged, 0x63c=3 and the ISP/PBE state (D3_CONST/D3_RD) below still run every 3D.
    a.i(f"ld.a.d r4, r17, #{PB_3D_LOADED} [moe]")
    a.i("wdf")
    a.i("test.xor.eq p0, r4, r11")                    # same PB already loaded -> skip
    a.i("(p0) ba skip_3d_fl")
    # save_ta_state: stash the TA page counters in the PB descriptor, shifted.
    a.i(f"ldr r4, #{0x700 // 4}")
    a.i(f"ldr r5, #{0x710 // 4}")
    a.i(f"ldr r6, #{0x724 // 4}")
    a.i(f"ldr r7, #{0x72c // 4}")
    a.i("wdf")
    a.i("shl r5, r5, #16")                            # CR0x710 << 16
    a.i("shr r6, r6, #16")                            # (CR0x724 & 0xffff0000) >> 16
    a.i("shr r7, r7, #16")                            # CR0x72c >> 16
    a.i("st.a.d r11, #%d, r4 [moe]" % (0x18 // 4))    # pb+0x18 = CR0x700
    a.i("st.a.d r11, #%d, r5 [moe]" % (0x1c // 4))    # pb+0x1c = CR0x710 << 16
    a.i("st.a.d r11, #%d, r6 [moe]" % (0x20 // 4))    # pb+0x20 = CR0x724 >> 16
    a.i("st.a.d r11, #%d, r7 [moe]" % (0x24 // 4))    # pb+0x24 = CR0x72c >> 16
    a.i("idf")
    a.i("wdf")
    # load_3d_state: 0x600/0x604/0x64c/0x660 from the PB descriptor.
    a.i("ld.a.d r4, r11, #%d [moe]" % (0x10 // 4))    # pb+0x10 (free-list address)
    a.i("wdf")
    a.i("shr r4, r4, #4")                             # (pb+0x10) & ~0xf
    a.i("shl r4, r4, #4")
    a.i(f"str #{0x600 // 4}, r4")
    a.i("ld.a.d r4, r11, #%d [moe]" % (0x18 // 4))    # pb+0x18
    a.i("ld.a.d r5, r11, #%d [moe]" % (0x1c // 4))    # pb+0x1c
    a.i("ld.a.d r6, r11, #%d [moe]" % (0x20 // 4))    # pb+0x20
    a.i("ld.a.d r7, r11, #%d [moe]" % (0x24 // 4))    # pb+0x24
    a.i("wdf")
    a.i(f"str #{0x604 // 4}, r4")                     # 0x604 = pb+0x18
    a.i(f"str #{0x64c // 4}, r5")                     # 0x64c = pb+0x1c
    a.i("shl r7, r7, #16")                            # pb+0x24 << 16
    a.i("or r6, r6, r7")                              # pb+0x20 | (pb+0x24 << 16)
    a.i(f"str #{0x660 // 4}, r6")                     # 0x660
    a.dpm_freelist(5, CR_DPM_3D_FL, 0x2)             # 0x664=5, 0x680=1, wait: load the 3D free list
    a.i(f"st.a.d r17, #{PB_3D_LOADED}, r11 [moe]")   # remember the loaded PB
    a.i("idf")
    a.i("wdf")
    a.label("skip_3d_fl")
    # OOM partial render (SC_OOM==2): the DPM slot shuffle.
    # Render a COPY of the TA's partly-binned DPM state in the OTHER slot (1) so the 3D frees that
    # copy's pages while the TA keeps its live slot (0) to continue binning the same scene. Without
    # it the 3D freed the TA's own live slot -> the resumed scene read freed pages (the 7th-OOM fault
    # 0x1f0b8000). One scene at a time -> the other slot is always empty: no eviction, no ctlblk slot
    # words (SC_INFLIGHT/SC_XFER untouched). r10/r11 (SC_RD/SC_PB) survive for the register program.
    a.i(f"ld.a.d r4, r17, #{SC_OOM} [moe]")
    a.i("wdf")
    a.i("test.xor.ne p0, r4, #2")                     # SC_OOM != 2 -> normal render, skip the shuffle
    a.i("(p0) ba skip_oom_shuffle")
    a.i(f"ld.a.d r2, r17, #{SC_RTENTRY} [moe]")       # rtentry
    a.i("wdf")
    a.i("ld.a.d r3, r2, #5 [moe]")                    # rtentry word 5 (+0x14)
    a.i("wdf")
    # Slot-1 control-init: the shuffle renders slot 1, so the 3D's frees go through
    # slot 1's DPM control (0x698/0x634). We never run a real scene in slot 1, so that control is whatever
    # reset left -> frees link pages off the TA's chain (leak: count recovers, ring short). Give slot 1 a
    # valid control ONCE, like a DDK first kick into slot 1: 0x630=5, 0x674=w5, 0x634=w6, 0x694=0x698=2,
    # wait 0x240. Gated on SLOT1_INIT so it runs only before the first shuffle.
    a.i(f"ld.a.d r4, r17, #{SLOT1_INIT} [moe]")
    a.i("wdf")
    a.i("test.and.ne p0, r4, r4")                     # slot-1 control already set up -> straight to shuffle
    a.i("(p0) ba skip_slot1_init")
    a.i("ld.a.d r5, r2, #6 [moe]")                    # rtentry word 6 (+0x18)
    a.i("wdf")
    a.i(f"ldr r7, #{0x630 // 4}")                     # save 0x630 (survives a.wait; r5/r6 don't)
    a.i("wdf")
    a.i("limm r4, #5")                                # 0x630 = 5: slot 1 (b0|b2), as a DDK first kick
    a.i(f"str #{0x630 // 4}, r4")
    a.i(f"str #{0x674 // 4}, r3")                     # slot-1 state register = rtentry w5
    a.i(f"str #{0x634 // 4}, r5")                     # slot-1 control register = rtentry w6
    a.i("limm r4, #2")
    a.i(f"str #{CR_DPM_STATE // 4}, r4")              # 0x694 = 2: load the slot-1 state
    a.i(f"str #{CR_DPM_CONTROL // 4}, r4")            # 0x698 = 2: load the slot-1 control
    a.wait(CR_EVENT_STATUS, 0x240, CR_EVENT_HOST_CLEAR)  # b6 state + b9 control load done
    a.i(f"str #{0x630 // 4}, r7")                     # restore 0x630 (the shuffle re-selects below)
    a.i("limm r4, #1")
    a.i(f"st.a.d r17, #{SLOT1_INIT}, r4 [moe]")       # slot-1 control is now valid; do this once
    a.label("skip_slot1_init")
    a.i("limm r4, #2")                                # 0x630 = 2: TA slot 0, 3D slot 1
    a.i(f"str #{0x630 // 4}, r4")
    a.i(f"str #{0x62c // 4}, r3")                     # slot-0 register = rtentry w5
    # NO 0x65c = 1 on the OOM terminate path: the DDK never sets DPM load-select here (sa8 b1 is always set by
    # the terminate path, so it skips both 0x65c=1 writes) -- r65c = 0 in all 1.04M DDK rows; ours set it in 4.
    # With 0x65c = 1 the 0x698 = 2 control clear at teardown widened to the free-list bookkeeping and dropped
    # ring entries the final free never relinked (page count back to 0 but the ring short -> the MT-onset dry).
    # (The paired 0x65c = 0 below is now a no-op; kept so 0x65c stays 0.)
    a.i("limm r4, #1")
    a.i(f"str #{CR_DPM_STATE // 4}, r4")              # 0x694 = 1: store the TA slot's partial state
    a.wait(CR_EVENT_STATUS, 0x10, CR_EVENT_HOST_CLEAR)
    a.i("limm r4, #2")
    a.i(f"str #{CR_DPM_STATE // 4}, r4")              # 0x694 = 2: clear the TA slot (TA keeps binning)
    a.wait(CR_EVENT_STATUS, 0x40, CR_EVENT_HOST_CLEAR)   # b6 state-clear done (0x694-only; NOT the 0x240 combined)
    a.i("limm r4, #3")                                # 0x630 = 3: select the other slot (1)
    a.i(f"str #{0x630 // 4}, r4")
    a.i(f"str #{0x674 // 4}, r3")                     # slot-1 register = rtentry w5
    a.i("limm r4, #4")
    a.i(f"str #{CR_DPM_STATE // 4}, r4")              # 0x694 = 4: load the stored state into slot 1
    a.wait(CR_EVENT_STATUS, 0x20, CR_EVENT_HOST_CLEAR)   # load done (0x20/0x100100)
    a.i("limm r4, #2")                                # 0x630 = 2: back to the TA's slot 0
    a.i(f"str #{0x630 // 4}, r4")
    a.i("limm r4, #0")
    a.i(f"str #{CR_DPM_LOADSEL // 4}, r4")            # 0x65c = 0
    a.label("skip_oom_shuffle")
    a.consts(D3_CONST)
    # OOM partial render: 0x658 = 1 (D3_CONST leaves it 0 on the normal path).
    a.i(f"ld.a.d r4, r17, #{SC_OOM} [moe]")
    a.i("wdf")
    a.i("test.xor.ne p0, r4, #2")
    a.i("(p0) ba skip_oom_658")
    a.i("limm r4, #1")
    a.i(f"str #{0x658 // 4}, r4")                     # 0x658 = 1
    a.label("skip_oom_658")
    a.program(10, D3_RD)
    # OOM partial render (SC_OOM==2): redirect the "next" region headers (0x50c) to THIS render's
    # (0x408), so the partial render RESETS the lists it binned; the TA then resumes into empty lists
    # and the scene's final render re-bins. Without this the final render walks the rendered lists a
    # second time and the DPM frees their pages AGAIN -> r724 high-half underflow (0xedf9) + hang.
    # D3_RD set 0x50c = rd+0x28 (the normal "other" array); override here.
    a.i(f"ld.a.d r4, r17, #{SC_OOM} [moe]")
    a.i("wdf")
    a.i("test.xor.ne p0, r4, #2")                     # SC_OOM != 2 -> normal, keep D3_RD's 0x50c
    a.i("(p0) ba skip_oom_50c")
    a.i(f"ldr r4, #{0x408 // 4}")                     # this render's region headers
    a.i("wdf")
    a.i(f"str #{0x50c // 4}, r4")                     # 0x50c = 0x408
    a.label("skip_oom_50c")
    # D: 3D flags. 0xa74's 3D field (b22-26) = the render details'
    # word 0x13c (0x07c00000), masked REPLACE (clear b22-26, OR in) so a stale 3D
    # field from a previous scene is cleared, not accumulated; keeps the TA field
    # (0x50000000) and srvinit's 0x208200 -> 0x57e08200. Then 0x658 = 0; 0x43c = 1.
    a.i(f"ldr r4, #{0xa74 // 4}")
    a.i("ld.a.d r5, r10, #%d [moe]" % (0x13c // 4))  # rd+0x13c = 3D flags (b22-26)
    a.i("limm r6, #0xf83fffff")                       # ~0x07c00000: keep all but the 3D field
    a.i("wdf")
    a.i("and r4, r4, r6")
    a.i("or r4, r4, r5")                             # a74 = (a74 & ~3D-field) | flags
    a.i(f"str #{0xa74 // 4}, r4")
    # 0x658 = 0 only on the NORMAL path. do_3d clears 0x658 twice (D3_CONST, then here); the OOM override
    # above set it to 1, but THIS second clear undid it, so the partial render ran as a NORMAL render for its
    # whole duration (r658=1 only ~15us) -> the 3D treated the scene as finished and the second resume died.
    # Keep 0x658 = 1 through the OOM partial render; only the teardown clears it.
    a.i(f"ld.a.d r4, r17, #{SC_OOM} [moe]")
    a.i("wdf")
    a.i("test.xor.eq p0, r4, #2")                     # SC_OOM == 2 -> skip this 0x658 = 0
    a.i("(p0) ba skip_oom658_clear")
    a.i("limm r5, #0")
    a.i(f"str #{0x658 // 4}, r5")                     # 0x658 = 0 (normal render)
    a.label("skip_oom658_clear")
    a.i("limm r5, #1")
    a.i(f"str #{0x43c // 4}, r5")
    # invalidate the 3D-side caches and wait. On the overlap queued route this do_3d
    # can run with the 3D units clock-gated off (render_complete -> do_3d), so use the
    # DDK order (MADD waited first, PDS/USE right before the 0x4b spin); the serial path
    # keeps the serial invalidate() (units warm from the TA).
    if OVERLAP:
        a.invalidate3d()
    else:
        a.invalidate(CR_PDS_INV1, INV_DONE_3D)
    # C (DDK): the background-object PDS words. The client
    # leaves bgblock[0..2] = 0, so without these the ISP's background object runs a
    # zeroed PDS program and the 3D hangs (lockup_3d, BIF_FAULT 0). bgblock VA =
    # *(rtentry+0x10); word0/1 = TA cmd+0x08/+0x0c; word2 = the kernel-data list
    # the PDS primary points at, *(*(rd+0x114) + 4*rd[0x108]). Route the 3D
    # requestor to list 1 (EUR_CR_BIF_BANK0 b12-15 = 1 -> 0x101f = the client PD),
    # then pulse the EDM field to list 1 too (0x1011) so the EDM can write the
    # client-PD bgblock, and back to 0x101f (EDM=15), with an idf/wdf fence so the
    # writes land before the EDM returns to list 15. The DDK c78 at 3D =
    # 0x101f, bg pulse 0x1011->0x101f, c84 never touched.
    # BANK0: route the 3D requestor to list 1 (the client PD) -- the render needs this
    # whether or not the EDM writes the bgblock below.
    if MULTI_MEM_CONTEXTS:
        a.i("limm r5, #0x101f")                       # BANK0: EDM=15, TA=1, 3D -> list 1
        a.i(f"str #{0xc78 // 4}, r5")
        a.i(f"ldr r5, #{0xc78 // 4}")                 # read-back + fence
        a.i("wdf")
    # Background-object PDS block. The EDM writes bgblock[0..2] into the CLIENT PD, which
    # needs a transient EDM->list-1 pulse (0x1011) -- but that routes the EDM's own
    # instruction fetch onto the client PD and faults it under secure/no-share
    # (KERNEL_CODE+0x20d0). When the host wrote the block on the CPU it sets
    # HOSTCTL_HOST_BGBLOCK (hostctl word 15 bit 0, UKERNEL.md §4a); skip the whole
    # pulse + write then (its three words are known at submit). The DDK/srvinit path has
    # the flag clear and does the pulse. Skipping also avoids the kernel-value loads below,
    # which the EDM would otherwise read while routed to the client PD.
    a.i(f"ld.a.d r5, r17, #{SA_HOSTCTL} [moe]")
    a.i("wdf")
    a.i(f"ld.a.d r5, r5, #{HC_BGBLOCK} [moe]")        # hostctl word 15 (+0x3c)
    a.i("wdf")
    a.i("and r5, r5, #1")                             # bit 0 = HOSTCTL_HOST_BGBLOCK
    a.i("test.and.ne p0, r5, r5")
    a.i("(p0) ba skip_bgblock")                       # host wrote it -> skip the EDM pulse
    a.i(f"ld.a.d r12, r17, #{SC_RTENTRY} [moe]")      # RT-data entry
    a.i(f"ld.a.d r9, r17, #{SC_CMD} [moe]")           # TA command base
    a.i("wdf")
    a.i("ld.a.d r2, r12, #%d [moe]" % (0x10 // 4))    # bgblock VA = *(rtentry+0x10)
    a.i("ld.a.d r3, r9, #%d [moe]" % (0x08 // 4))     # cmd+0x08  (bg PDS secondary)
    a.i("ld.a.d r4, r9, #%d [moe]" % (0x0c // 4))     # cmd+0x0c  (bg PDS middle)
    a.i("ld.a.d r6, r10, #%d [moe]" % (0x114 // 4))   # rd+0x114 = kernel-data list base
    a.i("ld.a.d r7, r10, #%d [moe]" % (0x108 // 4))   # rd+0x108 = index
    a.i("wdf")
    a.i("imae r6, r7, #4, r6 [t2=u32]")               # list base + 4*index
    a.i("ld.a.d r6, r6, #0 [moe]")                    # word2 (bg PDS primary)
    a.i("wdf")
    if MULTI_MEM_CONTEXTS:                            # one context: the EDM is already on the job's PD
        a.i("limm r5, #0x1011")                       # BANK0: EDM -> list 1 for the bg stores
        a.i(f"str #{0xc78 // 4}, r5")
        a.i(f"ldr r5, #{0xc78 // 4}")                 # read-back + fence
        a.i("wdf")
    a.i("st.a.d r2, #0, r3 [moe]")                    # bgblock[0] = cmd+0x08
    a.i("st.a.d r2, #1, r4 [moe]")                    # bgblock[1] = cmd+0x0c
    a.i("st.a.d r2, #2, r6 [moe]")                    # bgblock[2] = list word
    a.i("idf")
    a.i("wdf")
    if MULTI_MEM_CONTEXTS:
        a.i("limm r5, #0x101f")                       # BANK0: EDM -> list 15, 3D stays list 1
        a.i(f"str #{0xc78 // 4}, r5")
        a.i(f"ldr r5, #{0xc78 // 4}")
        a.i("wdf")
    a.label("skip_bgblock")
    # arm the completion rendezvous: this render is not done until BOTH the 3D writeout
    # (pixel_done, 0x40000 + ISP idle) and the DPM free (free_done, 0x1) have happened.
    a.i("limm r5, #1")
    a.i(f"st.a.d r17, #{WAIT_3D}, r5 [moe]")
    a.i(f"st.a.d r17, #{WAIT_FREE}, r5 [moe]")
    a.i("idf")
    a.i("wdf")
    # start the 3D
    a.i("limm r5, #1")
    a.i(f"str #{CR_3D_KICK // 4}, r5")
    if FRAMEPROF:
        a.i(f"ld.a.d r4, r17, #{FP_TICK} [moe]"); a.i("wdf"); a.st_sa(SA_SELF, FP_3D_KICK, 4)  # frameprof: 3D kick (render path)
    a.end()

    # ======================================================================
    # pixel_done: 0x40000 (pixel back-end) / 0x800000 (ISP end) -- rendezvous half 1
    # ======================================================================
    # The 3D has finished rendering. The writeout is truly done only once the ISP clock
    # goes idle; wait for that, clear WAIT_3D, and complete the render ONLY if WAIT_FREE
    # (the DPM free, 0x1) has already cleared -- else end and let free_done complete it.
    # A TRANSFER completes here directly (no rendezvous; its bookkeeping differs).
    a.label("pixel_done")
    a.i(f"ld.a.d r1, r17, #{SC_XFER} [moe]")
    a.i("wdf")
    a.i("test.and.ne p0, r1, r1")
    a.i("(p0) ba xfer_done")
    a.i(f"ld.a.d r1, r17, #{SC_INFLIGHT} [moe]")      # a render in flight?
    a.i("wdf")
    a.i("test.and.eq p0, r1, r1")
    a.i("(p0) ba pixel_done_end")                     # no render: spurious 3D-end
    # ISP-idle rendezvous: if CLKGATECTL&3==2 (clock-on-demand; 0xA6AA,
    # so always), poll CLKGATESTATUS (0x008) bit 0 until it reads 0. Bounded,
    # so a stuck clock can never wedge the EDM; after the bound we proceed anyway.
    a.i(f"ldr r5, #{CR_CLKGATECTL // 4}")              # EUR_CR_CLKGATECTL
    a.i("wdf")
    a.i(f"limm r7, #{CLKGATE_ISP_MASK}"); a.i("and r5, r5, r7")
    a.i(f"limm r7, #{CLKGATE_ISP_AUTO}"); a.i("test.xor.ne p0, r5, r7") # ISP not on demand -> no ISP wait
    a.i("(p0) ba pixel_isp_done")
    a.i("limm r6, #0")                                 # spin counter
    a.label("pixel_isp_loop")
    a.i(f"ldr r5, #{CR_CLKGATESTATUS // 4}")           # EUR_CR_CLKGATESTATUS
    a.i("wdf")
    a.i(f"limm r7, #{CLKGATESTATUS_ISP}"); a.i("and r5, r5, r7")   # ISP clock bit
    a.i("test.and.eq p0, r5, r5")                     # bit 0 == 0 -> ISP idle
    a.i("(p0) ba pixel_isp_done")
    if OVERLAP:
        # UNBOUNDED spin, as the DDK does (PBE_END handler). The bounded version below can proceed
        # with the ISP still clocking on a big render; serially that's harmless (the next 3D is a whole TA
        # away), but under overlap render_complete -> do_3d(N+1) writes the 0x428 kick immediately, and the HW
        # DROPS it while the ISP is still clocked -> the queued 3D never triggers (EVENT_STATUS2=0, no clock) =
        # the silent hang. So the completion must wait for the 3D pipe to be truly idle.
        a.i("ba pixel_isp_loop")
    else:
        a.i("imae r6, r6, #1, #1 [t2=u32]")           # counter += 1
        a.i("limm r7, #4096")
        a.i("test.xor.ne p0, r6, r7")                 # under the bound -> keep polling
        a.i("(p0) ba pixel_isp_loop")
    a.label("pixel_isp_done")
    a.i("limm r4, #0")
    a.i(f"st.a.d r17, #{WAIT_3D}, r4 [moe]")           # WAIT_3D = 0 (3D writeout done)
    a.i("idf")
    a.i("wdf")
    a.i(f"ld.a.d r1, r17, #{WAIT_FREE} [moe]")         # DPM free already done?
    a.i("wdf")
    a.i("test.and.eq p0, r1, r1")
    a.i("(p0) ba render_complete")                     # both halves done -> complete
    a.label("pixel_done_end")
    a.i("idf")
    a.i("wdf")
    a.end()                                            # 3D writeout done; await 0x1

    # ======================================================================
    # free_done: 0x1 (DPM 3D memory free) -- rendezvous half 2
    # ======================================================================
    # Ack 0x1 (not acked up-front, so a coincident 0x1 re-raises), clear WAIT_FREE, and
    # complete the render ONLY if WAIT_3D (the 3D writeout) has already cleared -- else
    # end and let pixel_done complete it. A 0x1 with no render in flight (SC_INFLIGHT==0:
    # a transfer's free, or a spurious event) has nothing to complete -- ack and end.
    a.label("free_done")
    if WAIT_LOG:
        a.i("limm r4, #4"); a.st_sa(SA_SELF, 0x3a, 4)   # stage: reached free_done
    if DEBUG_SENTINEL:
        a.i("limm r6, #0x4D04"); a.st_sa(SA_SELF, 0x3A, 6)
    a.i("limm r4, #1")
    a.i(f"str #{CR_EVENT_HOST_CLEAR // 4}, r4")        # ack EVENT_STATUS bit 0 (0x1)
    a.i(f"ld.a.d r1, r17, #{SC_INFLIGHT} [moe]")      # a render in flight?
    a.i("wdf")
    a.i("test.and.eq p0, r1, r1")
    a.i("(p0) ba render_done_noop")                   # no render: a transfer/spurious free
    a.i("limm r4, #0")
    a.i(f"st.a.d r17, #{WAIT_FREE}, r4 [moe]")         # WAIT_FREE = 0 (pages freed)
    a.i("idf")
    a.i("wdf")
    a.i(f"ld.a.d r1, r17, #{WAIT_3D} [moe]")           # 3D writeout already done?
    a.i("wdf")
    a.i("test.and.eq p0, r1, r1")
    a.i("(p0) ba render_complete")                     # both halves done -> complete
    a.i("idf")
    a.i("wdf")
    a.end()                                            # pages freed; await 0x40000

    # ======================================================================
    # render_complete: both halves of the rendezvous done -- complete ONCE
    # ======================================================================
    # Walk the render's HW device sync list, set each destination's write-ops-complete
    # to pending+1 (docs/UKERNEL.md §5), tear the render down, and interrupt the host.
    a.label("render_complete")
    if FRAMEPROF:
        a.i(f"ld.a.d r4, r17, #{FP_TICK} [moe]"); a.i("wdf"); a.st_sa(SA_SELF, FP_3D_FIN, 4)  # frameprof: 3D finished
        a.i(f"ld.a.d r5, r17, #{FP_3D_KICK} [moe]"); a.i("wdf")
        a.i("test.isub16.ne p2, r4, r4, r5")          # r4 = (tick - 3D_kick) & 0xffff = this render's 3D busy ticks
        a.i(f"ld.a.d r6, r17, #{FP_3D_BUSY} [moe]"); a.i("wdf")
        # running total is the full-32-bit addend (imae truncates the multiplicands to 16 bits)
        a.i("imae r6, r4, #1, r6 [t2=u32]"); a.i(f"st.a.d r17, #{FP_3D_BUSY}, r6 [moe]")   # 3D-busy total += delta
        a.i("limm r5, #256")                          # 256*256 = 0x10000: add to the HIGH 16 of the packed counts
        a.i(f"ld.a.d r6, r17, #{FP_COUNTS} [moe]"); a.i("wdf")
        a.i("imae r6, r5, r5, r6 [t2=u32]"); a.i(f"st.a.d r17, #{FP_COUNTS}, r6 [moe]")    # 3D count (high 16) += 1
    # OOM partial render done (SC_OOM==2): UNPAUSE the TA FIRST, then close the
    # DPM control and resume the allocation (0x69c=1), and SKIP the scene teardown -- the scene isn't done;
    # the TA keeps binning (in its live slot 0) into the pages this partial render freed from slot 1.
    # ORDER MATTERS (DDK ground truth): the DDK unpauses BEFORE 0x698=2/0x69c=1 so the
    # TA is live when the control clear + resume land, and it climbs straight back to the threshold to re-OOM.
    # My earlier order unpaused LAST, leaving the TA paused through the control clear -> on HW the resumed TA
    # grabbed only ~2 pages then stalled below threshold with no new OOM.
    a.i(f"ld.a.d r1, r17, #{SC_OOM} [moe]")
    a.i("wdf")
    a.i("test.xor.ne p0, r1, #2")                     # SC_OOM != 2 -> normal teardown
    a.i("(p0) ba render_complete_oom1")               # SC_OOM != 2 -> finish-during-OOM check, then normal
    # Grant re-check, while the TA is still paused and the 3D has freed: did
    # this partial render free < 2 pages? If so set OOM_G (and OOM_G2 in the GBL case) so the NEXT OOM grants
    # reserve pages instead of rendering again. OOM_G is sticky until TA finish.
    a.i(f"ld.a.d r2, r17, #{OOM_MARK_TA} [moe]")
    a.i(f"ldr r3, #{0x724 // 4}")
    a.i("wdf")
    a.i("shr r3, r3, #16")                            # count after the free
    a.i("test.isub16.neg p0, r2, r3")                # mark_ta - count < 0 (count > mark_ta = freed < 2 TA)
    a.i("(!p0) ba rc_oom_gl")                         # TA freed >= 2 -> check the GBL mark
    a.i(f"ld.a.d r2, r17, #{OOM_FLAGS} [moe]")
    a.i("wdf")
    a.i(f"or r2, r2, #{OOM_G}")
    a.i(f"st.a.d r17, #{OOM_FLAGS}, r2 [moe]")
    a.i("ba rc_oom_flag_done")
    a.label("rc_oom_gl")
    a.i(f"ld.a.d r2, r17, #{OOM_MARK_GL} [moe]")
    a.i(f"ldr r3, #{0x72c // 4}")
    a.i("wdf")
    a.i("shr r3, r3, #16")
    a.i("test.isub16.neg p0, r2, r3")                # count > mark_gl -> set G|G2
    a.i("(!p0) ba rc_oom_flag_done")
    a.i(f"ld.a.d r2, r17, #{OOM_FLAGS} [moe]")
    a.i("wdf")
    a.i(f"or r2, r2, #{OOM_G | OOM_G2}")
    a.i(f"st.a.d r17, #{OOM_FLAGS}, r2 [moe]")
    a.label("rc_oom_flag_done")
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("shr r5, r5, #1")                             # unpause the TA FIRST (clear 0xa74 b0)
    a.i("shl r5, r5, #1")
    a.i(f"str #{0xa74 // 4}, r5")
    # NO 0x65c = 1 before the 0x698 = 2 control clear: the DDK never sets DPM load-select on the OOM terminate
    # path (r65c = 0 in all 1.04M DDK rows). With it set, the control clear dropped page-ring entries the final
    # free never relinked -> the MT-onset short-ring dry.
    a.i("limm r4, #2")
    a.i(f"str #{CR_DPM_CONTROL // 4}, r4")            # 0x698 = 2: DPM control clear
    a.i("limm r4, #0")
    a.i(f"str #{0x658 // 4}, r4")                     # 0x658 = 0
    a.wait(CR_EVENT_STATUS, 0x200, CR_EVENT_HOST_CLEAR)   # DPM_CONTROL_CLEAR
    a.i("limm r4, #0")
    a.i(f"str #{CR_DPM_LOADSEL // 4}, r4")            # 0x65c = 0
    a.i("limm r4, #1")
    a.i(f"str #{CR_DPM_OOM // 4}, r4")                # 0x69c = 1: resume the TA's allocation
    a.i("limm r4, #0")
    a.i(f"st.a.d r17, #{SC_OOM}, r4 [moe]")           # clear SC_OOM
    # an OOM fired during this episode (ack'd + queued in k_oom to avoid an EDM storm)? start a fresh
    # episode now on the resumed TA -- the DDK's post-partial re-check, so no OOM is ever lost.
    a.i(f"ld.a.d r1, r17, #{SC_OOM_PENDING} [moe]")
    a.i("wdf")
    a.i("test.and.eq p0, r1, r1")                     # no pending OOM -> done
    a.i("(p0) ba kick_end")                           # skip the scene teardown; the TA continues
    a.i("limm r4, #0")
    a.i(f"st.a.d r17, #{SC_OOM_PENDING}, r4 [moe]")   # clear the queued flag
    a.i("ba k_oom")                                   # re-terminate the resumed TA for the queued OOM
    a.label("render_complete_oom1")
    # SC_OOM==1 at a normal render completion = the TA FINISHED during an OOM pause (its last allocation hit
    # the limit, so it raised TA_FINISHED instead of TA_TERMINATE). This render just freed the scene's pages at
    # the DPM free, so NOW it is safe to release the held allocation: unpause + 0x69c=1 + clear SC_OOM. The old
    # escape (a) released at ta_finished, BEFORE the pages were freed, so the TA grabbed a page past the limit
    # into an exhausted free list and walked off the end -> BIF fault 0x1ffff000 (ep#1). The DDK
    # has no abort and never hits this (0/1206 TA_FINISHED during a pause); this path only fires if HW does.
    a.i(f"ld.a.d r1, r17, #{SC_OOM} [moe]")
    a.i("wdf")
    a.i("test.xor.ne p0, r1, #1")                     # SC_OOM != 1 -> normal render, nothing held
    a.i("(p0) ba render_complete_normal")
    a.i(f"ldr r5, #{0xa74 // 4}")
    a.i("wdf")
    a.i("shr r5, r5, #1")                             # unpause the TA (clear 0xa74 b0)
    a.i("shl r5, r5, #1")
    a.i(f"str #{0xa74 // 4}, r5")
    a.i("limm r4, #1")
    a.i(f"str #{CR_DPM_OOM // 4}, r4")                # 0x69c = 1: release the held allocation (pages are back)
    a.i("limm r4, #0")
    a.i(f"st.a.d r17, #{SC_OOM}, r4 [moe]")           # clear SC_OOM
    a.i(f"st.a.d r17, #{SC_OOM_PENDING}, r4 [moe]")   # any queued overlap is moot: the scene finished
    a.label("render_complete_normal")
    if not MULTI_MEM_CONTEXTS:
        a.restore_fw_pd()                           # before the first completion write
    if OVERLAP:
        a.i(f"ld.a.d r0, r17, #{TD_RD} [moe]")   # the completing 3D's render details (SC_RD may now be N+1's TA)
    else:
        a.i(f"ld.a.d r0, r17, #{SC_RD} [moe]")   # render details
    a.i("wdf")
    # Destination sync 0 (docs/UKERNEL.md §5 "3D end of render"): clear the
    # render's own sync ops-pending word (render-details word 0x34 = byte 0xD0 =
    # rd_sync, an H_SYNC object the EDM's kernel PD maps) so the next render on
    # this RT starts from zero pending ops.
    a.i("ld.a.d r2, r0, #%d [moe]" % RD_SYNC0)        # sync 0 = *(rd+0xD0) = rd_sync
    a.i("wdf")
    a.i("st.a.d r2, #0, #0 [moe]")                    # *sync0 = 0
    a.i("idf")
    a.i("wdf")
    # TA3D dependency block (render details word 56 = byte 0xE0). The DDK reads
    # all its other 3D-completion client writes from here (the DDK's memory
    # trace); if the block pointer is 0 none of them run.
    #   - sync 1 = *(*(deps+0x00)) cleared to 0 -- the destination access sync.
    #     This fires whenever deps != 0, regardless of the complete-addr below.
    #   - complete counter: *(deps+0x14) = *(deps+0x10) + 1 (single render, slot 0).
    # The OLD sync-1 took its address from rd+0x240, which is PAST the 0x1ec
    # render-details structure (STATE.md §6; 0x240 is a TA-command offset, not an
    # rd one) -- a stale/zero pointer the EDM (dir-list 15 = kernel PD) wrote 0
    # through: a BIF page fault at a client general-heap page or NULL = the
    # render hang (DDK renders 10/10 without that bug). All the DDK's
    # targets are SYNCINFO-heap (0x2d00xxxx), which the kernel PD maps.
    a.i("ld.a.d r2, r0, #56 [moe]")                   # deps = *(rd+0xE0)
    a.i("wdf")
    a.i("test.and.eq p0, r2, r2")                     # no deps -> skip sync1 + counter
    a.i("(p0) ba rd_wops")
    a.i("ld.a.d r3, r2, #0 [moe]")                    # sync 1 address = deps[0]
    a.i("wdf")
    a.i("test.and.eq p0, r3, r3")                     # null entry -> skip the clear
    a.i("(p0) ba rd_sync1_done")
    a.i("st.a.d r3, #0, #0 [moe]")                    # *sync1 = 0
    a.i("idf")
    a.i("wdf")
    a.label("rd_sync1_done")
    a.i("ld.a.d r4, r2, #4 [moe]")                    # pending  (deps + 0x10)
    a.i("ld.a.d r5, r2, #5 [moe]")                    # complete address (deps + 0x14)
    a.i("wdf")
    a.i("test.and.eq p0, r5, r5")                     # no complete address -> skip
    a.i("(p0) ba rd_wops")
    a.i("imae r4, r4, #1, #1 [t2=u32]")               # pending + 1
    a.i("st.a.d r5, #0, r4 [moe]")                    # *complete = pending + 1
    a.i("idf")
    a.i("wdf")
    a.label("rd_wops")
    # write-ops tracking: render details word 65 vs frame+1; reset word 66
    a.i("ld.a.d r2, r0, #65 [moe]")
    a.i("wdf")
    a.i("limm r3, #1")                                # frame index 0 + 1
    a.i("test.xor.ne p1, r2, r3")
    a.i("(!p1) st.a.d r0, #66, #0 [moe]")
    a.i("(p1) st.a.d r0, #66, r3 [moe]")
    a.i("idf")
    a.i("wdf")
    # Render-in-flight teardown / DPM 3D memory free (docs/UKERNEL.md §5). The
    # render has completed, so free its slot and RT and clear the in-flight
    # markers do_ta/do_3d set: mark the RT-data complete, move the render state
    # to "freed", release the RT, and empty the context and control-block in-
    # flight slots. One context: the in-flight "list" is the single control-
    # block slot, cleared to 0 (not an unlink), which is also what tells the
    # cleanup and power handlers no render is running.
    if OVERLAP:
        a.i(f"ld.a.d r1, r17, #{TD_ENTRY} [moe]")  # the completing 3D's RT entry (not N+1's TA)
        a.i(f"ld.a.d r2, r17, #{TD_CTX} [moe]")    # and its HW render context
    else:
        a.i(f"ld.a.d r1, r17, #{SC_RTENTRY} [moe]")  # RT-data entry
        a.i(f"ld.a.d r2, r17, #{SC_CTX} [moe]")      # HW render context
    a.i("wdf")
    a.i("ld.a.d r3, r1, #%d [moe]" % RT_DATA)            # rtdata = *(rtentry+0x30)
    a.i("wdf")
    a.i("limm r4, #1")
    a.i("st.a.d r3, #0, r4 [moe]")                       # rtdata[0] = 1 (render complete)
    a.i("limm r4, #0")
    a.i("st.a.d r1, #%d, r4 [moe]" % RT_RENDER)          # rtentry+0x4 = 0 (RT free)
    a.i("limm r4, #%d" % RT_IDLE_STATE)
    a.i("st.a.d r1, #%d, r4 [moe]" % RT_STATE)           # rtentry+0x34 = 0x10 (RT idle)
    a.i("limm r4, #0")
    a.i("st.a.d r2, #%d, r4 [moe]" % CTX_INFLIGHT0)      # ctx+0x28 = 0
    a.i("st.a.d r2, #%d, r4 [moe]" % CTX_INFLIGHT1)      # ctx+0x2c = 0
    a.i("st.a.d r2, #%d, r4 [moe]" % CTX_INFLIGHT_FLAG)  # ctx+0x3c = 0
    a.i("st.a.d r0, #%d, r4 [moe]" % RD_ACTIVE)          # rd+0x12c = 0 (render inactive)
    a.i(f"st.a.d r17, #{SC_INFLIGHT}, r4 [moe]")  # control block +0x50 = 0 (none in flight);
                                                  # this runs on 0x1 (after the page free), so
                                                  # clearing it lets a deferred cleanup free safely
    if OVERLAP:
        a.i(f"st.a.d r17, #{TD_ENTRY}, r4 [moe]")  # 3D done: clear the slot-pick gate (the next first kick's
                                                   # slot pick resets 0x630, so the 3D's b1 needn't be cleared here)
    a.i("limm r5, #%d" % RD_STATE)
    a.i("imae r5, r5, #1, r0 [t2=u32]")                  # &(rd+0x2a4)
    a.i("limm r4, #2")
    a.i("st.a.d r5, #0, r4 [moe]")                       # rd+0x2a4 = 2 (render freed; DDK does this at completion)
    a.i("idf")
    a.i("wdf")
    if OVERLAP:
        # a 3D queued behind this one (its TA finished while this 3D ran)? Raise this render's completion IRQ,
        # then start the queued 3D: do_3d reads SC_* = the queued scene, which credit=2 kept ours. Else normal end.
        a.i(f"ld.a.d r3, r17, #{PENDING_3D} [moe]")
        a.i("wdf")
        a.i("test.and.ne p0, r3, r3")
        a.i("(!p0) ba raise_host_irq")
        a.i("limm r3, #0")
        a.i(f"st.a.d r17, #{PENDING_3D}, r3 [moe]")       # dequeue
        a.i(f"limm r0, #{SW_EVENT}")
        a.i(f"str #{CR_EVENT_STATUS // 4}, r0")           # raise this render's completion IRQ (inline)
        a.i("ba do_3d")                                   # start the queued 3D on the freed 3D engine
    a.i("ba raise_host_irq")
    # render_done_noop: a DPM 3D-memory-free event (0x1) arrived with no render in
    # flight -- a transfer's own free, or a spurious/duplicate event. 0x1 is already
    # acked at render_done's head; there is nothing to complete, so just end.
    a.label("render_done_noop")
    a.i("idf")
    a.i("wdf")
    a.end()
    # ======================================================================
    # xfer_done: a transfer's 3D finished. Advance its context's CCB past the
    # command (so the next transfer is found) and clear the in-flight marker,
    # then raise the host interrupt. (The destination sync-object update from
    # the transfer's SGXMKIF_TRANSFERCMD_SHARED is the next increment; docs/TEX.md.)
    # ======================================================================
    a.label("xfer_done")
    if not MULTI_MEM_CONTEXTS:
        a.restore_fw_pd()                           # before the first completion write (keeps r1)
    a.i("ld.a.d r5, r1, #%d [moe]" % CTX_CCB)            # transfer context CCB base
    a.i("ld.a.d r2, r1, #%d [moe]" % CTX_CCBCTL)         # CCB control
    a.i("wdf")
    a.i("ld.a.d r3, r2, #1 [moe]")                       # read offset (this command)
    a.i("wdf")
    a.i("imae r6, r3, #1, r5 [t2=u32]")                  # r6 = transfer command base
    # status word (sCtlStatusInfo: command 0x168 = address, 0x16c = value). Write it when
    # present. This is NOT the transfer's done signal -- that's asDstSyncs[0] below (§10) --
    # it's optional and a blit can leave 0x168 = 0. GUARD it: writing to a null status
    # address faults at ~0 and never reaches the dst-sync write (BIF_FAULT
    # 0x0f, transfer CCB not advanced, dst sync unwritten -- the status write was the null
    # deref; the blit's 0x168 was 0 while its real sync is 0x140 = 0x2d000004).
    a.i("ld.a.d r7, r6, #%d [moe]" % XFER_SYNC_ADDR)     # status address (cmd+0x168)
    a.i("ld.a.d r8, r6, #%d [moe]" % XFER_SYNC_VAL)      # status value (cmd+0x16c)
    a.i("wdf")
    a.i("test.and.eq p0, r7, r7")                        # null status address -> skip
    a.i("(p0) ba xfer_status_done")
    a.i("st.a.d r7, #0, r8 [moe]")
    a.i("idf")
    a.i("wdf")
    a.label("xfer_status_done")
    # asDstSyncs[0] (SGXMKIF_TRANSFERCMD_SHARED word 0x134): set its write-ops-complete to
    # write-ops-pending + 1, the done signal powervr_sgx (and the DDK) actually poll for a
    # transfer -- the status word above (0x168/0x16c) is NOT it (UKERNEL.md §10). Layout:
    # 0x134 read-ops pending, 0x138 read-complete addr, 0x13c write-ops pending, 0x140
    # write-ops-complete addr. render_complete does the same for a render's deps sync.
    # The blit rendered right but the done word never changed without this.
    a.i("ld.a.d r9, r6, #%d [moe]" % (0x13c // 4))       # asDstSyncs[0] write-ops pending
    a.i("ld.a.d r10, r6, #%d [moe]" % (0x140 // 4))      # write-ops-complete address
    a.i("wdf")
    a.i("test.and.eq p0, r10, r10")                      # no complete addr -> skip
    a.i("(p0) ba xfer_dstsync_done")
    a.i("imae r9, r9, #1, #1 [t2=u32]")                  # pending + 1
    a.i("st.a.d r10, #0, r9 [moe]")                      # *write-ops-complete = pending + 1
    a.i("idf")
    a.i("wdf")
    a.label("xfer_dstsync_done")
    a.i("limm r4, #%d" % TRANSFERCMD_BYTES)
    a.i("imae r3, r4, #1, r3 [t2=u32]")                  # read offset += command size
    a.i("st.a.d r2, #1, r3 [moe]")                       # advance the CCB read offset
    a.i("limm r4, #0")
    a.i("st.a.d r1, #%d, r4 [moe]" % CTX_XFER0)          # clear ctx in-flight fields
    a.i("st.a.d r1, #%d, r4 [moe]" % CTX_XFER1)
    a.i(f"st.a.d r17, #{SC_XFER}, r4 [moe]")      # no transfer in flight
    a.i("idf")
    a.i("wdf")
    a.label("raise_host_irq")
    if not MULTI_MEM_CONTEXTS:
        a.restore_fw_pd()                           # and on the idle path, in case
    a.i(f"limm r0, #{SW_EVENT}")
    a.i(f"str #{CR_EVENT_STATUS // 4}, r0")        # raise the host IRQ
    # After a completion (render_complete/xfer_done just cleared SC_INFLIGHT/SC_XFER),
    # service a command the host left queued in the kernel CCB right now instead of waiting
    # for the next 1 kHz timer tick (the DDK re-enters kick after completion).
    # Safe here, NOT in kick_end: the completion cleared the in-flight markers, so k_ta
    # won't defer the queued kick and there is no defer-loop; and the idle/active-power
    # path reaches this label only with an empty CCB (WO == RO), so it is a no-op there.
    a.i(f"ld.a.d r5, r17, #{SA_KCCBCTL} [moe]")
    a.i("wdf")
    a.i("ld.a.d r6, r5, #0 [moe]")                 # kernel CCB write offset
    a.i("ld.a.d r7, r5, #1 [moe]")                 # read offset
    a.i("wdf")
    a.i("test.xor.ne p0, r6, r7")                  # WO != RO -> a command is queued
    a.i("(p0) ba kick")                            # drain it now, don't wait a tick
    a.end()

    if PROBES:
        # bring-up only (cores/sgx530_probe.py): one probe per page past the image,
        # for the host to map at the address another code-base unit would start at.
        # Each writes its own EVENT_TIMER value (b24 clear) and ends
        page = 0x1000 // 8
        for k in PROBES:
            n = sum(1 for kind, _ in a.lines if kind != "label")
            a.lines.extend([("raw", (0, 0))] * (-n % page))   # zeros up to the page
            a.label(f"probe_{k}")
            a.i(f"limm r0, #{PROBE_TIMER + k}")
            a.i(f"str #{CR_EVENT_TIMER // 4}, r0")
            a.end()

    if BASE_PADS:
        # bring-up only (cores/sgx530_bases.py): a pad per USE code base, on
        # 128 B boundaries past the image, for the host to point code base n at.
        # Each writes EVENT_TIMER = PAD_TIMER + n (b24 clear) and ends
        for n in BASE_PADS:
            k = sum(1 for kind, _ in a.lines if kind != "label")
            a.lines.extend([("raw", (0, 0))] * (-k % 16))   # 16 instructions = 128 B
            a.label(f"pad_{n}")
            a.i(f"limm r0, #{PAD_TIMER + n}")
            a.i(f"str #{CR_EVENT_TIMER // 4}, r0")
            a.end()

    return a


def program():
    return build(Asm())
