# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""openHC's own SGX545 USSE microkernel, in assembly.

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

# ---- control-block field word indices (as secondary attributes) -----------
SA_SELF = 0          # the control block's own device address
SA_HOSTCTL = 1       # SGXMKIF_HOST_CTL address
SA_KCCBCTL = 2       # kernel CCB control (write/read offsets)
SA_KCCB = 0x16       # kernel CCB base, control block +0x58. This is the DDK's
                     # own slot: srvinit writes it at t[0x16] and the DDK model
                     # keeps it there (tools/ukernel/sgxsim.py, +0x58). (It read
                     # word 16 = +0x40 before — an unset slot — so the kick read
                     # the command from a garbage address and wedged the core;
                     # EA1 HW-confirmed 2026-11-02.)
# our one-context scratch (control block words we own; written to memory so the
# next task's DMA brings them back)
SC_CTX = 0x30
SC_CMD = 0x31
SC_RD = 0x32
SC_PB = 0x33
SC_RTENTRY = 0x34
SC_INFLIGHT = 0x14   # control block +0x50: the RT-data entry of the render in
                     # flight (0 = none). The DDK keeps a doubly-linked render-
                     # in-flight list; one context needs only this single slot,
                     # at the DDK's own offset so the A/B reference still matches.
SC_XFER = 0x15       # control block +0x54: the transfer context in flight (0 =
                     # none). Lets the 3D-end handler tell a transfer's completion
                     # from a render's. (openHC's own slot; a transfer and a
                     # render never overlap on one context.)
SC_VERSION = 0x18    # control block +0x60: openHC microkernel version, written
                     # at init. Advisory (the DDK driver does not read it); an
                     # openHC-aware kernel logs it (docs/UKERNEL-DECISIONS.md).
                     # A free slot: it was 0x16 before, which is the kernel CCB
                     # base — init overwrote that base with the version word.
OPENHC_UK_VERSION = 0x00010000   # 1.0
TRANSFERCMD_BYTES = 0x1c0   # SGXMKIF_TRANSFERCMD size (docs/TEX.md §3)
# transfer completion: the shared command's destination-sync descriptor and the
# transfer context's in-flight fields (docs/TEX.md; observed from a capture)
XFER_SYNC_ADDR = 0x168 // 4   # command word: destination sync write-complete address
XFER_SYNC_VAL = 0x16c // 4    # command word: the value to write (write-ops-complete)
CTX_XFER0 = 0x14 // 4         # transfer context in-flight fields, cleared on completion
CTX_XFER1 = 0xb0 // 4

# ---- render-details / RT-data / context fields touched at render teardown ----
# (docs/UKERNEL.md §5; byte offsets, observed from the DDK's completion writes)
RD_SYNC0 = 0xD0 // 4         # 3D destination sync object 0 (its status word cleared)
RD_SYNC1 = 0x240 // 4        # 3D destination sync object 1
RD_WRITEOPS = 0x108 // 4     # render-details write-ops word
RD_ACTIVE = 0x12C // 4       # render-details active flag (cleared when freed)
RD_STATE = 0x2A4             # render-details state: 1 when the 3D starts, 2 when freed
RT_RENDER = 0x4 // 4         # RT-data entry: the render using this RT (0 = free)
RT_DATA = 0x30 // 4          # RT-data entry word holding the RT-data pointer
RT_STATE = 0x34 // 4         # RT-data entry state flag
RT_IDLE_STATE = 0x10         # the value that flag holds when the RT is idle
CTX_INFLIGHT0 = 0x28 // 4    # HW context: the render in flight (two words + a flag)
CTX_INFLIGHT1 = 0x2C // 4
CTX_INFLIGHT_FLAG = 0x3C // 4

# ---- host control block (SGXMKIF_HOST_CTL) word offsets --------------------
HC_INIT = 0
HC_POWER = 1         # ui32PowerStatus
HC_CLEANUP = 2       # ui32CleanupStatus
HC_INTFLAGS = 8      # ui32InterruptFlags (microkernel -> host); bit 1 = ACTIVE_POWER
HC_INTCLEAR = 9      # ui32InterruptClearFlags; bit 0 = INTERRUPT_HWR
EDM_INIT_COMPLETE = 1
EDM_INTERRUPT_HWR = 1
EDM_INTERRUPT_ACTIVE_POWER = 2   # host control ui32InterruptFlags bit 1: the
                                 # microkernel is idle, so the host may power the
                                 # core down (sgx_mkif_km.h
                                 # PVRSRV_USSE_EDM_INTERRUPT_ACTIVE_POWER)
POWERCMD_POWEROFF = 1
POWERCMD_IDLE = 2
POWERCMD_RESUME = 3
CTXSUSPCMD_RESUME = 2

# ---- HW render context (SGX_REGISTER_HW_RENDER_CONTEXT) word offsets --------
CTX_PD = 2           # page directory phys (kernel-filled)
CTX_CCB = 3          # client CCB base
CTX_CCBCTL = 4       # client CCB control (write/read offsets, bytes)

# ---- SGXMKIF_COMMAND (kernel CCB entry, 32 bytes) --------------------------
CMD_SERVICE = 0      # = command type (our host-kick table is the identity)
CMD_DATA0 = 2        # ui32Data[0]
CMD_DATA1 = 3        # ui32Data[1] = HW render context for a TA
CMD_WORDS = 8        # 32 bytes

# command types (SGXMKIF_CMD_TYPE); our host-kick table is the identity
CMD_TA = 0
CMD_TRANSFER = 1
CMD_POWER = 3
CMD_CTXSUSPEND = 4
CMD_CLEANUP = 5
CMD_GETMISCINFO = 6

# ---- get-misc-info: the SGXMKIF version/feature surface and struct sizes ----
# PVRSRV_SGX_MISCINFO_INFO, written into the misc buffer. The version words are
# the SGXMKIF interface identity the kernel's DDK-version check validates
# (docs/UKERNEL-DECISIONS.md: we stay SGXMKIF-compatible); the struct sizes are
# the same ones srvinit declares in SGX_BRIDGE_INIT_INFO (docs/UKERNEL.md).
MISC_READY = 0x1
MISC_GET_STRUCT_SIZES = 0x2
MISC_VER3 = 0x10711          # misc[3]
MISC_BUILD = 862890          # misc[4] = DDK build 1.7.862890
MISC_CORE = 545              # misc[5]
MISC_VER6 = 0x1000D          # misc[6]
MISC_BUILDOPTS = 0x60218     # misc[7] = ui32ClientBuildOptions
# misc word -> struct size (SGX_MISCINFO_STRUCT_SIZES)
MISC_SIZES = [(8, 0x2b0), (9, 0x1e0), (10, 0x4c8), (11, 0xdc), (12, 0xd0), (13, 0x3c),
              (14, 0x44), (15, 0x1ec), (16, 0x38), (17, 0x50), (18, 0xb4), (19, 0x44), (20, 0x20)]
CR_CORE_ID = 0x1C
CR_CORE_REVISION = 0x20

# ---- SGXMKIF_CMDTA client command header word offsets (docs/STATE.md §1) ---
CMDTA_SIZE = 0x00 // 4
CMDTA_PB = 0x24 // 4         # PB descriptor
CMDTA_RD = 0x38 // 4         # render details
CMDTA_RTENTRY = 0x44 // 4    # RT-data entry
CMDTA_SHARED = 0x58          # ui32CCBOffset points here; command base is 0x58 before

# ---- EUR_CR registers (byte offset) ---------------------------------------
CR_EVENT_STATUS = 0x12C
CR_EVENT_HOST_CLEAR = 0x134
CR_EVENT_STATUS2 = 0x118
CR_EVENT_HOST_CLEAR2 = 0x114
CR_TA_KICK = 0x200           # starts the VDM/TA (unnamed in the GPL header)
CR_3D_KICK = 0x428           # starts the 3D (unnamed)
CR_USE_INVAL = 0xA08         # USE cache invalidate
CR_PDS_INV0 = 0xAD0          # PDS cache invalidate, TA side
CR_PDS_INV1 = 0xAD4          # PDS cache invalidate, 3D side
CR_PDS_INV_CSC = 0xAE0       # coefficient-store invalidate
CR_DPM_STATE = 0x694
CR_DPM_CONTROL = 0x698
CR_DPM_FL_MODE = 0x664       # DPM free-list load selector (TA = 3, 3D = 5)
CR_DPM_TA_FL = 0x684         # DPM TA free-list load  -> EUR_CR_EVENT_STATUS2 bit 0
CR_DPM_3D_FL = 0x680         # DPM 3D free-list load  -> EUR_CR_EVENT_STATUS2 bit 1
CR_OTPM = 0x24C
CR_TPC = 0x224
CR_EVENT_HOST_CLEAR_0x140 = 0x140    # the invalidate-done acknowledge register
INV_DONE_TA = 0x27           # 0x138 bits for the TA-side invalidate
INV_DONE_3D = 0x4B           # 0x138 bits for the 3D-side invalidate
SW_EVENT = 0x4000
DCU_INVALID = 0x400          # EUR_CR_EVENT_STATUS2: a data-cache-unit invalidate
                             # completion. Pending after the kernel's SysFinalise
                             # on the EA1; the event program must acknowledge it
                             # or it re-triggers forever (docs/UKERNEL.md §2).

# ---- register-programming tables (reg byte-offset, base word index) --------
# base = pb (PB descriptor); docs/UKERNEL.md §6 "parameter-buffer setup"
# (reg, word) or (reg, word, or_mask): the global-threshold register gets a
# mode bit OR'd in on top of the PB-descriptor word
TA_PB = [(0x618, 0x10 // 4), (0x61c, 0x18 // 4), (0x648, 0x1c // 4), (0x638, 0x20 // 4),
         (0x620, 0x28 // 4), (0x624, 0x2c // 4), (0x628, 0x30 // 4, 0x10000), (0x668, 0x34 // 4)]
# base = cmd (TA command); the TE/MTE layout from the command tail (+0x238..)
TA_TE = [(0x204, 0x238 // 4), (0x208, 0x23c // 4), (0x20c, 0x240 // 4), (0x210, 0x244 // 4),
         (0x214, 0x248 // 4), (0x218, 0x24c // 4), (0x238, 0x250 // 4), (0x23c, 0x254 // 4),
         (0x21c, 0x258 // 4), (0x220, 0x25c // 4), (0x240, 0x260 // 4), (0x244, 0x264 // 4),
         (0x248, 0x268 // 4), (0xa04, 0x26c // 4), (0xaa0, 0x270 // 4), (0xc90, 0x274 // 4),
         (0x2b0, 0x278 // 4), (0x2d4, 0x27c // 4), (0x2d0, 0x280 // 4), (0x2b4, 0x284 // 4),
         (0x324, 0x288 // 4), (0x368, 0x28c // 4), (0x364, 0x290 // 4), (0x370, 0x294 // 4),
         (0x374, 0x298 // 4), (0x74c, 0x29c // 4), (0x2a8, 0x2a0 // 4), (0x230, 0x2a4 // 4)]
# base = rtentry (RT-data entry)
TA_RT = [(0x62c, 0x14 // 4), (0x634, 0x18 // 4), (0x234, 0x1c // 4)]
# base = rd (render details)
TA_RD = [(0x274, 0x124 // 4)]
# base = rd; the ISP/PBE 3D state (docs/UKERNEL.md §7 "3D start")
D3_RD = [(0xcac, 0x00 // 4), (0xcb0, 0x04 // 4), (0x400, 0x08 // 4), (0x414, 0x0c // 4),
         (0x42c, 0x10 // 4), (0x41c, 0x14 // 4), (0x420, 0x18 // 4), (0x424, 0x1c // 4),
         (0x408, 0x20 // 4), (0x148, 0x24 // 4), (0x50c, 0x28 // 4), (0x480, 0x2c // 4),
         (0x484, 0x30 // 4), (0x488, 0x34 // 4), (0x48c, 0x38 // 4), (0x490, 0x3c // 4),
         (0x4b8, 0x40 // 4), (0x4bc, 0x44 // 4), (0x4c4, 0x48 // 4), (0x520, 0x4c // 4),
         (0x4c8, 0x50 // 4), (0x4dc, 0x54 // 4), (0x800, 0x58 // 4), (0xa5c, 0x5c // 4),
         (0xa60, 0x60 // 4), (0xa64, 0x64 // 4), (0x418, 0x68 // 4), (0xa84, 0x6c // 4),
         (0xa88, 0x70 // 4), (0xa8c, 0x74 // 4), (0xa90, 0x78 // 4), (0xa94, 0x7c // 4),
         (0xa98, 0x80 // 4), (0xb24, 0x84 // 4), (0xb28, 0x88 // 4), (0xb2c, 0x8c // 4),
         (0xa9c, 0x90 // 4), (0x820, 0x94 // 4), (0x824, 0x98 // 4), (0x828, 0x9c // 4),
         (0x82c, 0xa0 // 4), (0x834, 0xa4 // 4), (0x838, 0xa8 // 4), (0x83c, 0xac // 4),
         (0x840, 0xb0 // 4), (0x830, 0xb4 // 4), (0x844, 0xb8 // 4), (0x848, 0xbc // 4),
         (0x860, 0xc0 // 4), (0x864, 0xc4 // 4), (0x868, 0xc8 // 4), (0x4e8, 0xcc // 4),
         (0x73c, 0x108 // 4)]
# fixed 3D-start constants the DDK writes with immediates (not from a struct)
D3_CONST = [(0x63c, 3), (0x494, 0xff), (0x40c, 0), (0x410, 0), (0x404, 0),
            (0x658, 0), (0x43c, 1)]

# ---- transfer (SGX_SUBMITTRANSFER): a 3D render with only a background object
# (docs/TEX.md §3). The command is 0x1c0 bytes in the transfer context's CCB;
# client words 0..15 program the ISP/PBE 3D state below (reg byte-offset, command
# word). The shared part (kernel-filled syncs) sits at command+0xac.
T3_CMD = [(0x4c4, 0), (0x4bc, 1), (0x4b8, 2), (0x404, 3), (0x408, 4), (0x414, 5), (0xcac, 6),
          (0x42c, 7), (0x4c8, 8), (0xa5c, 9), (0xa60, 10), (0xa64, 11), (0x40c, 12), (0x410, 13),
          (0x400, 14), (0xab8, 15), (0x148, 16)]
# word 12 is the first tile ((x/32)<<16 | y/16) and 13 the last tile; a full-surface
# blit has first tile 0, a sub-rectangle blit does not (confirmed against a capture
# with the textures session). Word 4 (depth/stencil block) is 0 in every capture.
# fixed transfer-start constants (the render's 0x63c=3 is 0 for a transfer; no PB)
T3_CONST = [(0x480, 0), (0x63c, 0), (0x43c, 1)]
TRANSFERCMD_SHARED = 0xac     # ui32SharedCmdCCBOffset points here; command base is 0xac before


class Asm:
    def __init__(self):
        self.lines = []

    def label(self, name):
        self.lines.append(("label", name))

    def i(self, text):
        self.lines.append(("i", text))

    def patch(self, reg, name):
        """a limm the srvinit loader fills with a device address at load time
        (the DDK's own mechanism for dynamic addresses). build.py records its
        offset and `name` in the manifest; the instruction is `limm rN, #0`."""
        self.lines.append(("patch", (reg, name)))

    # --- building blocks -----------------------------------------------
    def end(self):
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
        self.i(f"limm r6, #{mask}")
        self.label(lbl)
        self.i(f"ldr r5, #{poll // 4}")
        self.i("wdf")
        self.i("and r5, r5, r6")
        self.i(f"test.xor.ne p0, r5, r6")
        self.i(f"(p0) ba {lbl}")
        self.i(f"str #{ack // 4}, r6")

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

    def dpm_freelist(self, mode, kick_reg, done_bit):
        """load a DPM free list so the geometry/render can allocate parameter-
        buffer pages: selector 0x664 = `mode` (TA 3, 3D 5), then pulse the TA
        (0x684) or 3D (0x680) load register, and wait for the done event in
        EUR_CR_EVENT_STATUS2 (TA bit 0, 3D bit 1). Without this the free list is
        empty, the parameter buffer's pages-in-use stays 0 and the TA never runs
        — EA1 HW-confirmed 2026-11-02 (docs/UKERNEL.md §6 "Load the free-list")."""
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
        self.wait(CR_EVENT_STATUS, 0x100000, CR_EVENT_HOST_CLEAR)
        self.wait(CR_EVENT_STATUS2, 0x1000, CR_EVENT_HOST_CLEAR2)


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
    a.phas()
    a.patch(17, "ctlblk")                          # r17 = control block base, reserved for the
                                                   # whole task. A task can write its own secondary
                                                   # attributes, but only after the emit-triggered
                                                   # pds1 DMA sets sa up; this event program is a
                                                   # bare DOUTU with no such DMA, so sa is empty on
                                                   # entry (writing it here wedges init, EA3 HW-
                                                   # confirmed 2026-11-02). The handlers therefore
                                                   # derive host-control / CCB pointers from r17.
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
    # §2); openHC only dispatched STATUS bits, so the invalidate-done the
    # kernel leaves set after SysFinalise re-triggered the event program
    # forever — a storm that starved the SW_EVENT host kick and locked the
    # core (EA1 HW-confirmed 2026-11-02: STATUS2=0x400, event re-entered 16+
    # times, the kick never serviced). Clearing a bit that is not set is a
    # no-op, so the acknowledge is unconditional and no handler reads it.
    a.i(f"limm r3, #{DCU_INVALID}")
    a.i(f"str #{CR_EVENT_HOST_CLEAR2 // 4}, r3")
    # acknowledge the TA-finish (0x2000) and 3D-end (0x40000 pixel back-end,
    # 0x800000 ISP end-of-tile) events in EUR_CR_EVENT_STATUS the same way: the
    # dispatch() below only tests the r2 snapshot and branches, it never clears
    # the serviced bit, and neither did the handlers — so the TA-finish event
    # re-triggered ta_finished forever, starving the next host kick (kick 2's
    # render) so do_3d was never reached, and the watchdog stormed. EA1 HW-
    # confirmed 2026-11-03: ta_finish stuck set at every lockup dump, do_ta +
    # ta_finished markers present but do_3d absent. The DDK acks these in its
    # handlers (str 0x134 = 0x2000 at TA finish, = 0x40000 at 3D end). Clearing an
    # unset bit is a no-op, so the acknowledge is unconditional and precedes the
    # dispatches, which test the r2 snapshot taken above.
    a.i(f"limm r3, #{0x2000 | 0x40000 | 0x800000}")
    a.i(f"str #{CR_EVENT_HOST_CLEAR // 4}, r3")
    a.dispatch(0x2000, "ta_finished")              # a TA finished
    a.dispatch(0x40000, "render_done")             # pixel back-end end of render
    a.dispatch(0x800000, "render_done")            # ISP end of tile (same handler)
    # no hardware-event bit set: a host kick arrived via the EUR_CR_EVENT_KICK
    # doorbell (sgxutils.c `SGXScheduleCCBCommand`), which leaves no
    # EUR_CR_EVENT_STATUS bit — so look at the kernel CCB for a waiting command.
    # The `kick` handler returns immediately if the CCB is empty (write offset
    # == read offset). Gating this on SW_EVENT (0x4000) was wrong: that bit is
    # the microkernel->host signal (set by raise_host_irq, cleared by the host
    # ISR), never an incoming event, so the kick was never serviced — EA1
    # HW-confirmed 2026-11-02 (GETMISCINFO sat unconsumed with EVENT_STATUS = 0,
    # `sw_event: no`, the core looping through HWR).
    a.i("ba kick")

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
    a.i(f"limm r0, #{OPENHC_UK_VERSION}")
    a.i(f"st.a.d r17, #{SC_VERSION}, r0 [moe]")   # advisory openHC version word
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
    a.i("mov r16, r8 [mask=1]")                 # context
    a.i("savl r23")
    a.i("ba do_ta [link]")
    a.i("setl r23")
    a.i("ba kick_adv")
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
    a.i(f"ld.a.d r2, r17, #{SA_HOSTCTL} [moe]")
    a.i("wdf")
    a.i(f"ld.a.d r3, r2, #{HC_CLEANUP} [moe]")
    a.i("wdf")
    a.i("or r3, r3, #1")                        # CLEANUPCMD_COMPLETE
    a.i(f"st.a.d r2, #{HC_CLEANUP}, r3 [moe]")
    a.i("idf")
    a.i("wdf")
    a.i("ba kick_adv")
    # transfer (data[1] = HW transfer context): a 3D-only render whose single
    # background object samples the source and writes the destination (texture
    # uploads and the swap blit; docs/TEX.md §3). The transfer context carries a
    # CCB at the same word offsets as a render context; its command's words 0..15
    # program the 3D. Point the BIF at the context, invalidate the 3D-side
    # caches, program the 3D from the command, and kick it. Completion comes
    # back through the 3D-end event like any render.
    a.label("k_transfer")
    a.i("mov r16, r8 [mask=1]")                     # transfer context
    a.i("ld.a.d r1, r16, #%d [moe]" % CTX_CCB)      # client CCB base
    a.i("ld.a.d r2, r16, #%d [moe]" % CTX_CCBCTL)   # CCB control
    a.i("wdf")
    a.i("ld.a.d r3, r2, #1 [moe]")                  # read offset (bytes)
    a.i("wdf")
    a.i("imae r9, r3, #1, r1 [t2=u32]")             # r9 = transfer command base
    a.i("ld.a.d r4, r16, #%d [moe]" % CTX_PD)       # context page directory
    a.i("wdf")
    a.i(f"str #{0xc84 // 4}, r4")                   # BIF -> this context
    a.invalidate(CR_PDS_INV1, INV_DONE_3D)
    a.program(9, T3_CMD)
    a.consts(T3_CONST)
    a.st_sa(SA_SELF, SC_XFER, 16)                   # remember the transfer in flight
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
    # driven EDM — EA1 HW-confirmed 2026-11-02. ui32InterruptFlags is host-
    # control word 8; the host gates its power-down on ui32InterruptClearFlags,
    # so re-raising the bit is idempotent and never storms.
    a.i(f"ld.a.d r1, r17, #{SA_KCCBCTL} [moe]")
    a.i("wdf")
    a.i("ld.a.d r2, r1, #0 [moe]")                 # write offset
    a.i("ld.a.d r3, r1, #1 [moe]")                 # read offset
    a.i("wdf")
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
    # first-kick flag: TA command +0x04 bit 0 is bFirstKickOrResume (0x71), and
    # is clear on the render / bLastInScene kick (0x62). A scene's first kick does
    # the full per-scene setup (TA soft reset, PB descriptor, DPM free-list + state
    # load, OTPM, RT region registers, client page directory); re-running any of
    # that on a later kick re-initialises the parameter buffer / free list
    # mid-scene and HANGS the TA — EA1 HW 2026-11-03 (kick 2: lockup_ta, do_3d
    # unreached). The DDK does only a minimal kick there (ddk-ref "TA kick 2":
    # TE/MTE, invalidate, RD, flags, kick). r13 = 1 on the first kick, 0 on a
    # render kick; each setup block below is gated on it.
    a.i("ld.a.d r13, r9, #1 [moe]")                 # cmd+0x04 (word 1)
    a.i("wdf")
    a.i("and r13, r13, #1")                         # r13 = bFirstKickOrResume
    # soft-reset the TA (EUR_CR_SOFT_RESET 0x080 bit 2) on the first kick only, so
    # it begins the scene clean (the DDK does this early, ddk-ref line 22).
    a.i("test.and.eq p0, r13, r13")                 # render kick (r13 == 0)? skip
    a.i("(p0) ba skip_ta_reset")
    a.i("limm r5, #4")
    a.i(f"str #{0x80 // 4}, r5")
    a.i("limm r5, #0")
    a.i(f"str #{0x80 // 4}, r5")
    a.label("skip_ta_reset")
    # remember context, command, and the pointers the 3D phase needs
    a.i("ld.a.d r10, r9, #%d [moe]" % CMDTA_RD)     # render details
    a.i("ld.a.d r11, r9, #%d [moe]" % CMDTA_PB)     # PB descriptor
    a.i("ld.a.d r12, r9, #%d [moe]" % CMDTA_RTENTRY)  # RT entry
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
    # directory-list 0 (EUR_CR_BIF_DIR_LIST_BASE0 = 0xc84), pulse the BIF
    # directory-cache invalidate (EUR_CR_BIF_CTRL 0xc00 bit 3 = INVALDC), then
    # route the TA requestor to list 0 while the EDM stays on list 15
    # (EUR_CR_BIF_BANK0 = 0xc78 = 0xf: b0-3 EDM=15, b4-7 TA=0). Without BANK0 the
    # TA reads the control stream through the wrong page tables and faults on the
    # kernel-data/command memory — EA1 HW 2026-11-02: EUR_CR_BIF_FAULT =
    # 0x3200F00F (the TA command base). Then invalidate the PDS/USE caches.
    # client page directory (0xc84): first kick only — the render kick reuses the
    # scene's PD, and the DDK does not reload it (ddk-ref "TA kick 2").
    a.i("test.and.eq p0, r13, r13")                 # render kick (r13 == 0)? skip
    a.i("(p0) ba skip_bif_pd")
    a.i("ld.a.d r4, r16, #%d [moe]" % CTX_PD)
    a.i("wdf")
    a.i(f"str #{0xc84 // 4}, r4")                   # BIF_DIR_LIST_BASE0 = client PD
    a.label("skip_bif_pd")
    a.i("limm r5, #0xf")                            # BIF_BANK0: EDM=list 15, TA=list 0
    a.i(f"str #{0xc78 // 4}, r5")
    # directory-cache invalidate (INVALDC): first kick only.
    a.i("test.and.eq p0, r13, r13")                 # render kick? skip INVALDC
    a.i("(p0) ba skip_bif_invaldc")
    a.i("limm r5, #8")                              # BIF_CTRL: INVALDC (flush dir cache)
    a.i(f"str #{0xc00 // 4}, r5")
    a.i("limm r5, #0")
    a.i(f"str #{0xc00 // 4}, r5")
    a.label("skip_bif_invaldc")
    a.invalidate(CR_PDS_INV0, INV_DONE_TA)
    # clear 0xa74 before re-writing its TA-flags field below. The DDK clears it
    # (ddk-ref line 61) then writes the command's flags into b27-31; srvinit's
    # init value (0x208200) is overwritten. Without the write our 0xa74 keeps
    # only the init bits (b27-31 = 0) and the kicked TA does not run.
    a.i("limm r5, #0")
    a.i(f"str #{0xa74 // 4}, r5")
    # parameter-buffer registers + TA free-list load: first kick only. These set
    # up the scene's parameter buffer; re-running them on the render kick reloads
    # the free list the first kick already allocated pages from and hangs the TA
    # (the DDK skips them, ddk-ref "TA kick 2").
    a.i("test.and.eq p0, r13, r13")             # render kick? skip PB + free list
    a.i("(p0) ba skip_pb_fl")
    a.program(11, TA_PB)
    a.dpm_freelist(3, CR_DPM_TA_FL, 0x1)        # load the TA free list before the kick
    a.label("skip_pb_fl")
    # TE / macrotile / screen layout from the command tail
    a.program(9, TA_TE)
    # 0xa74 = the TA command's flags word (cmd+0x14), as the DDK writes it after
    # the TE registers and before the kick (ddk-ref line 91; STATE.md cmd+0x14 =
    # 0x50000000 when kick flag 0x2000000 is clear). Read +0x14 (word 5), not the
    # +0x18 constant.
    a.i(f"ld.a.d r9, r17, #{SC_CMD} [moe]")
    a.i("wdf")
    a.i("ld.a.d r5, r9, #5 [moe]")
    a.i("wdf")
    a.i(f"str #{0xa74 // 4}, r5")
    # RT region-header / tail-pointer base registers (0x62c/0x634/0x234): first
    # kick only — they describe the scene's parameter-buffer regions, set once and
    # reused (the DDK skips them on the render kick).
    a.i("test.and.eq p0, r13, r13")             # render kick? skip RT region regs
    a.i("(p0) ba skip_rt")
    a.program(12, TA_RT)
    a.label("skip_rt")
    a.program(10, TA_RD)
    # second cache invalidate (both kicks), then the DPM state/control load. The
    # load is first kick only: it loads the scene's DPM state, and re-running it
    # mid-scene disturbs the in-flight parameter state (the DDK skips it on the
    # render kick). Done before the TE registers, dpm_load's 0x240 done event
    # never fired and do_ta stalled — EA1 HW sentinels 2026-11-02 — so it stays
    # after them (ddk-ref-frame-registers.txt lines 106-131).
    a.invalidate(CR_PDS_INV0, INV_DONE_TA)
    a.i("test.and.eq p0, r13, r13")             # render kick? skip DPM state load
    a.i("(p0) ba skip_dpm_load")
    a.dpm_load()
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
    # start the TA
    a.i("limm r5, #1")
    a.i(f"str #{CR_TA_KICK // 4}, r5")
    a.i("lapc")

    # ======================================================================
    # ta_finished: write the TA status values, then program and start the 3D
    # ======================================================================
    a.label("ta_finished")
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
    # start the 3D once the scene's last TA has finished. CMDTA_SHARED's
    # ui32CtrlFlags (command word 0) bit 0 (READY) is set by the kernel only
    # for the last kick (bLastInScene), so a command with it set means render.
    a.i(f"ld.a.d r1, r17, #{SC_CMD} [moe]")   # TA command base
    a.i("wdf")
    a.i("ld.a.d r2, r1, #0 [moe]")                    # ui32CtrlFlags
    a.i("wdf")
    a.i("test.and.ne p0, r2, #1")                     # READY (last in scene)?
    a.i("(p0) ba do_3d")
    a.end()

    # ======================================================================
    # do_3d: program and start the 3D for the scene (run on the last TA's finish)
    # ======================================================================
    a.label("do_3d")
    a.i(f"ld.a.d r10, r17, #{SC_RD} [moe]")   # render details
    a.i(f"ld.a.d r11, r17, #{SC_PB} [moe]")   # PB descriptor
    a.i("wdf")
    # mark the context's in-flight render and move the render state to "3D
    # running" (the DDK sets these once the scene's last TA has finished and the
    # 3D is about to start); the completion handler clears them.
    a.i(f"ld.a.d r2, r17, #{SC_CTX} [moe]")
    a.i("wdf")
    a.i("st.a.d r2, #%d, r10 [moe]" % CTX_INFLIGHT0)  # ctx+0x28 = render details
    a.i("st.a.d r2, #%d, r10 [moe]" % CTX_INFLIGHT1)  # ctx+0x2c = render details
    a.i("limm r3, #1")
    a.i("limm r4, #%d" % RD_STATE)
    a.i("imae r4, r4, #1, r10 [t2=u32]")              # &(rd+0x2a4)
    a.i("st.a.d r4, #0, r3 [moe]")                    # rd+0x2a4 = 1 (3D running)
    a.i("idf")
    a.i("wdf")
    a.i("ld.a.d r4, r11, #%d [moe]" % (0x10 // 4))    # 0x600 <- pb+0x10
    a.i("wdf")
    a.i(f"str #{0x600 // 4}, r4")
    a.dpm_freelist(5, CR_DPM_3D_FL, 0x2)             # load the 3D free list before the render
    a.consts(D3_CONST)
    a.program(10, D3_RD)
    # invalidate the 3D-side caches and wait, then start the 3D
    a.invalidate(CR_PDS_INV1, INV_DONE_3D)
    a.i("limm r5, #1")
    a.i(f"str #{CR_3D_KICK // 4}, r5")
    a.end()

    # ======================================================================
    # render_done: update the destination sync objects and raise the host IRQ
    # ======================================================================
    # The 3D-end handler walks the render's HW device sync list and sets each
    # destination's write-ops-complete to pending+1 (docs/UKERNEL.md §5). The
    # list walk is the next increment; the host-interrupt tail is here.
    a.label("render_done")
    # a transfer in flight completes through this same 3D-end event, but its
    # bookkeeping differs from a render's; route it to xfer_done.
    a.i(f"ld.a.d r1, r17, #{SC_XFER} [moe]")
    a.i("wdf")
    a.i("test.and.ne p0, r1, r1")
    a.i("(p0) ba xfer_done")
    a.i(f"ld.a.d r0, r17, #{SC_RD} [moe]")    # render details
    a.i("wdf")
    # HW device sync list (SGXMKIF_HWDEVICE_SYNC_LIST): the render's destination
    # sync objects (here the colour and depth destinations, addressed by render-
    # details words 0x34 and 0x90). Clear each one's ops-pending word now the 3D
    # has drained, so the next render on this RT starts from zero pending ops
    # (docs/UKERNEL.md §5 "3D end of render").
    a.i("ld.a.d r2, r0, #%d [moe]" % RD_SYNC0)        # sync object 0 address
    a.i("limm r5, #%d" % 0x240)
    a.i("imae r3, r5, #1, r0 [t2=u32]")               # &(rd+0x240)
    a.i("ld.a.d r3, r3, #0 [moe]")                    # sync object 1 address
    a.i("wdf")
    a.i("st.a.d r2, #0, #0 [moe]")                    # *sync0 = 0
    a.i("st.a.d r3, #0, #0 [moe]")                    # *sync1 = 0
    a.i("idf")
    a.i("wdf")
    # TA3D dependency sync: the destination the host/next client polls. The
    # render details hold a pointer to the dependency block (word 56); its
    # sync object has {write-ops-pending at +0x10, write-ops-complete-addr at
    # +0x14}. Set *complete-addr = pending + 1 (docs/UKERNEL.md §5). Single
    # render: frame index 0, so no per-frame stride.
    a.i("ld.a.d r2, r0, #56 [moe]")                   # dependency block
    a.i("wdf")
    a.i("test.and.eq p0, r2, r2")                     # none -> skip
    a.i("(p0) ba rd_wops")
    a.i("ld.a.d r4, r2, #4 [moe]")                    # pending  (block + 0x10)
    a.i("ld.a.d r5, r2, #5 [moe]")                    # complete address (block + 0x14)
    a.i("wdf")
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
    a.i(f"st.a.d r17, #{SC_INFLIGHT}, r4 [moe]")  # control block +0x50 = 0 (none in flight)
    a.i("limm r5, #%d" % RD_STATE)
    a.i("imae r5, r5, #1, r0 [t2=u32]")                  # &(rd+0x2a4)
    a.i("limm r4, #2")
    a.i("st.a.d r5, #0, r4 [moe]")                       # rd+0x2a4 = 2 (render freed)
    a.i("idf")
    a.i("wdf")
    a.i("ba raise_host_irq")
    # ======================================================================
    # xfer_done: a transfer's 3D finished. Advance its context's CCB past the
    # command (so the next transfer is found) and clear the in-flight marker,
    # then raise the host interrupt. (The destination sync-object update from
    # the transfer's SGXMKIF_TRANSFERCMD_SHARED is the next increment; docs/TEX.md.)
    # ======================================================================
    a.label("xfer_done")
    a.i("ld.a.d r5, r1, #%d [moe]" % CTX_CCB)            # transfer context CCB base
    a.i("ld.a.d r2, r1, #%d [moe]" % CTX_CCBCTL)         # CCB control
    a.i("wdf")
    a.i("ld.a.d r3, r2, #1 [moe]")                       # read offset (this command)
    a.i("wdf")
    a.i("imae r6, r3, #1, r5 [t2=u32]")                  # r6 = transfer command base
    # destination sync: write the write-ops-complete value the kernel put in the
    # command (word 0x16c) to the sync's complete-address (word 0x168), so the
    # client sees the blit done (docs/TEX.md). (The transfer's other sync-heap
    # objects come from the kick's sync-handle list -- the next increment.)
    a.i("ld.a.d r7, r6, #%d [moe]" % XFER_SYNC_ADDR)     # destination sync address
    a.i("ld.a.d r8, r6, #%d [moe]" % XFER_SYNC_VAL)      # write-ops-complete value
    a.i("wdf")
    a.i("st.a.d r7, #0, r8 [moe]")
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
    a.i(f"limm r0, #{SW_EVENT}")
    a.i(f"str #{CR_EVENT_STATUS // 4}, r0")
    a.end()

    return a


def program():
    return build(Asm())
