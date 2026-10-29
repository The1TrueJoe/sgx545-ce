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
SA_KCCB = 16         # kernel CCB base
# our one-context scratch (control block words we own; written to memory so the
# next task's DMA brings them back)
SC_CTX = 0x30
SC_CMD = 0x31
SC_RD = 0x32
SC_PB = 0x33
SC_RTENTRY = 0x34
SC_RIFLIST = 0x3C    # render-in-flight list head (we keep it empty: 0 or 1 entry)

# ---- host control block (SGXMKIF_HOST_CTL) word offsets --------------------
HC_INIT = 0
HC_INTCLEAR = 9      # ui32InterruptClearFlags; bit 0 = INTERRUPT_HWR
EDM_INIT_COMPLETE = 1
EDM_INTERRUPT_HWR = 1

# ---- HW render context (SGX_REGISTER_HW_RENDER_CONTEXT) word offsets --------
CTX_PD = 2           # page directory phys (kernel-filled)
CTX_CCB = 3          # client CCB base
CTX_CCBCTL = 4       # client CCB control (write/read offsets, bytes)

# ---- SGXMKIF_COMMAND (kernel CCB entry, 32 bytes) --------------------------
CMD_SERVICE = 0      # = command type (our host-kick table is the identity)
CMD_DATA1 = 3        # ui32Data[1] = HW render context for a TA
CMD_WORDS = 8        # 32 bytes

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
CR_DPM_STATE = 0x694
CR_DPM_CONTROL = 0x698
CR_OTPM = 0x24C
CR_TPC = 0x224
SW_EVENT = 0x4000

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


class Asm:
    def __init__(self):
        self.lines = []

    def label(self, name):
        self.lines.append(("label", name))

    def i(self, text):
        self.lines.append(("i", text))

    # --- building blocks -----------------------------------------------
    def end(self):
        self.i("nop [end]")

    def phas(self):
        self.i("phas.i #0, #0 [wait=7]")

    def ld_sa(self, dst, saword, off):
        """dst = *(saword + off), a device load through a secondary attribute"""
        self.i(f"mov r{dst}, sa{saword} [mask=1]")
        self.i(f"ld.a.d r{dst}, r{dst}, #{off} [moe]")
        self.i("wdf")

    def st_sa(self, saword, off, src):
        """*(saword + off) = r<src>"""
        self.i(f"mov r0, sa{saword} [mask=1]")
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

    def pulse_wait(self, reg, val, poll, bit):
        """write reg=val, then spin until (poll & bit) is set, then end the spin.
        The bit mask goes in a register (test immediates are only 7 bits)."""
        self.i(f"limm r6, #{bit}")
        self.i(f"limm r5, #{val}")
        self.i(f"str #{reg // 4}, r5")
        self.label(f"_w_{reg:x}_{poll:x}")
        self.i(f"ldr r5, #{poll // 4}")
        self.i("wdf")
        self.i("and r5, r5, r6")
        self.i("test.xor.ne p0, r5, r6")
        self.i(f"(p0) ba _w_{reg:x}_{poll:x}")


def build(a):
    # ======================================================================
    # init entry
    # ======================================================================
    # Clean boot: set ui32InitStatus and end. Recovery (HWR bit set): discard
    # the surviving kernel CCB and render-in-flight list first (docs/UKERNEL.md
    # §5a) so no stale, possibly-dead work is replayed, then set init status.
    a.label("init")
    a.phas()
    a.ld_sa(1, SA_HOSTCTL, HC_INTCLEAR)        # r1 = ui32InterruptClearFlags
    a.i(f"test.and.ne p0, r1, #{EDM_INTERRUPT_HWR}")
    a.i("(!p0) ba init_set")                   # clean boot: skip the discard
    # recovery: kernel CCB read offset := write offset (drop stale commands)
    a.i(f"mov r1, sa{SA_KCCBCTL} [mask=1]")
    a.i("ld.a.d r2, r1, #0 [moe]")             # write offset
    a.i("wdf")
    a.i("st.a.d r1, #1, r2 [moe]")             # read offset := write offset
    a.i("idf")
    a.i("wdf")
    a.i("limm r2, #0")                          # render-in-flight list head := empty
    a.st_sa(SA_SELF, SC_RIFLIST, 2)
    a.label("init_set")
    a.i(f"limm r0, #{EDM_INIT_COMPLETE}")
    a.i(f"st.a.d sa{SA_HOSTCTL}, #{HC_INIT}, r0 [moe]")
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
    a.phas()
    a.i(f"mov r1, sa{SA_KCCBCTL} [mask=1]")     # kernel CCB control
    a.i("ld.a.d r2, r1, #0 [moe]")              # write offset (slot index)
    a.i("ld.a.d r3, r1, #1 [moe]")              # read offset (slot index)
    a.i("wdf")
    a.i("test.xor.eq p0, r2, r3")               # empty?
    a.i("(p0) ba kick_end")
    # command = kernel CCB base + read_offset * 32
    a.i(f"mov r6, sa{SA_KCCB} [mask=1]")
    a.i(f"imae r6, r3, #{CMD_WORDS * 4}, r6 [t2=u32]")   # r6 = &command (bytes)
    a.i(f"ld.a.d r7, r6, #{CMD_SERVICE} [moe]")          # type
    a.i(f"ld.a.d r8, r6, #{CMD_DATA1} [moe]")            # data[1] = context
    a.i("wdf")
    a.i("test.and.ne p0, r7, r7")               # type 0 = TA?
    a.i("(p0) ba kick_adv")                     # non-TA: nothing else this version
    a.i("mov r16, r8 [mask=1]")                 # pass context to do_ta
    a.i("savl r23")
    a.i("ba do_ta [link]")
    a.i("setl r23")
    a.label("kick_adv")
    # read offset := (read offset + 1) & 0xff
    a.i(f"mov r1, sa{SA_KCCBCTL} [mask=1]")
    a.i("ld.a.d r3, r1, #1 [moe]")
    a.i("wdf")
    a.i("imae r3, r3, #1, #1 [t2=u32]")
    a.i("and r3, r3, #255")
    a.i("st.a.d r1, #1, r3 [moe]")
    a.i("idf")
    a.i("wdf")
    a.label("kick_end")
    a.end()

    # ======================================================================
    # do_ta: program and start the TA for the context in r16 (subroutine)
    # ======================================================================
    a.label("do_ta")
    # client CCB and its control, then the TA command base
    a.i("ld.a.d r1, r16, #%d [moe]" % CTX_CCB)      # client CCB base
    a.i("ld.a.d r2, r16, #%d [moe]" % CTX_CCBCTL)   # client CCB control
    a.i("wdf")
    a.i("ld.a.d r3, r2, #1 [moe]")                  # client read offset (bytes)
    a.i("wdf")
    a.i("imae r9, r3, #1, r1 [t2=u32]")             # r9 = command base (bytes)
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
    # point the BIF at this context's page directory
    a.i("ld.a.d r4, r16, #%d [moe]" % CTX_PD)
    a.i("wdf")
    a.i(f"str #{0xc84 // 4}, r4")
    # parameter-buffer registers, then the DPM state/control load request.
    # NOTE: this first version pulses the DPM load and does not yet wait for
    # its completion events (docs/UKERNEL.md §6); the completion handshake is
    # the next increment and needs its own hardware validation. The struct-
    # sourced register programming below is what run_ours.py checks.
    a.program(11, TA_PB)
    a.i("limm r5, #2")
    a.i(f"str #{CR_DPM_STATE // 4}, r5")
    a.i(f"str #{CR_DPM_CONTROL // 4}, r5")
    # TE / macrotile / screen layout from the command tail
    a.program(9, TA_TE)
    a.program(12, TA_RT)
    a.program(10, TA_RD)
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
    a.phas()
    # (the TA status-value writes to the sync heap are a per-command list; the
    #  single-triangle frame has them, but reproducing the list walk is the
    #  next increment -- see run_ours.py's diff, which covers the registers)
    a.i(f"ld.a.d r10, sa{SA_SELF}, #{SC_RD} [moe]")   # render details
    a.i(f"ld.a.d r11, sa{SA_SELF}, #{SC_PB} [moe]")   # PB descriptor
    a.i("wdf")
    a.i("ld.a.d r4, r11, #%d [moe]" % (0x10 // 4))    # 0x600 <- pb+0x10
    a.i("wdf")
    a.i(f"str #{0x600 // 4}, r4")
    a.consts(D3_CONST)
    a.program(10, D3_RD)
    # start the 3D
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
    a.phas()
    a.label("raise_host_irq")
    a.i(f"limm r0, #{SW_EVENT}")
    a.i(f"str #{CR_EVENT_STATUS // 4}, r0")
    a.end()

    return a


def program():
    return build(Asm())
