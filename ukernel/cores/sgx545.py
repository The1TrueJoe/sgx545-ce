# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Per-core microkernel configuration for the SGX545 (docs/UKERNEL-TOOLING.md §3).

The register map, control-block layout, host-control and command offsets, and
the register-programming tables that make the microkernel program this core.
ukernel.py imports these; another SGX core gets its own cores/<core>.py with the
same names and its own values, and nothing in ukernel.py's handler logic changes.
The SGX545 serial image stays byte-identical across this move
(ukernel/golden/sgx545.golden, checked by the ukernel-golden CI).
"""

# ---- control-block field word indices (as secondary attributes) -----------
SA_SELF = 0          # the control block's own device address
SA_HOSTCTL = 1       # SGXMKIF_HOST_CTL address
SA_KCCBCTL = 2       # kernel CCB control (write/read offsets)
SA_KCCB = 0x16       # kernel CCB base, control block +0x58. This is the DDK's
                     # own slot: srvinit writes it at t[0x16] and the DDK model
                     # keeps it there (tools/ukernel/sgxsim.py, +0x58). (It read
                     # word 16 = +0x40 before — an unset slot — so the kick read
                     # the command from a garbage address and wedged the core.)
# our one-context scratch (control block words we own; written to memory so the
# next task's DMA brings them back)
SC_CTX = 0x30
SC_CMD = 0x31
SC_RD = 0x32
SC_PB = 0x33
SC_RTENTRY = 0x34
# Cleanup serialisation (the DDK's cleanup trace). A process-exit cleanup must
# not complete (let the host free the PB/RT) until this process's render AND its DPM
# page-release have finished. render_done now runs on the DPM 3D memory-free event
# (EVENT_STATUS bit 0, the DDK order): it is the event that fires AFTER the scene's 3D
# pages are freed, so when it clears SC_INFLIGHT the page release is already complete and
# the host sees "done" (sync objects + IRQ) only then. k_cleanup therefore defers on
# SC_INFLIGHT alone. CLEANUP_DEFER_CT bounds the defer (N timer ticks then force-complete
# + log) so a lost event can never stick the box.
CLEANUP_DEFER_CT = 0x1c
CLEANUP_DEFER_MAX = 256      # ~256 ms at 1 kHz; a render completes in << 30 ms
# Render-completion RENDEZVOUS (the DDK's completion trace). A render completes
# only once BOTH halves are done: the 3D writeout (0x40000 pixel back-end, after the ISP
# clock goes idle) AND the DPM 3D memory free (0x1). do_3d sets both flags at 3D start;
# pixel_done clears WAIT_3D, free_done clears WAIT_FREE, and whichever runs second (sees
# the other already clear) does the completion. Completing on 0x1 alone (the earlier
# build) let the client exit while the 3D writeout was still active -> the next process
# kicked into a live 3D (BANK0 0x101F at every fault) = the 5/10 alternation.
WAIT_3D = 0x1d               # set at 3D start, cleared by pixel_done (0x40000 + ISP idle)
WAIT_FREE = 0x1e             # set at 3D start, cleared by free_done (0x1 DPM free)
SC_OOM_PENDING = 0x1a  # control block +0x68: an OOM (EVENT_STATUS 0x6) fired while an episode was already
                     # in flight. We ack it (so it can't storm the EDM) and set this; the episode's teardown
                     # re-dispatches k_oom for it. The DDK's post-partial re-check (step 2), storm-free.
OOM_SAVED_628 = 0x1b # control block +0x6c: the global DPM threshold (0x628) saved before a grant raises it.
                     # ta_finished restores it (the DDK undoes the grant at TA finish).
                     # 0 = no grant outstanding.
# DDK grant (the scene-finish mechanism). Each partial render frees only ~20% of the binned
# pages; the DDK does 2-4 partial renders, and when one frees < 2 pages it takes the GRANT path on the NEXT
# OOM -- raise the TA/GBL thresholds and reload the free list -- so a dense scene can finish, then undoes it
# at TA finish. These flags/marks live in the control block (the DDK keeps them in sa8 bits + TA3D words).
OOM_FLAGS = 0x1f     # control block +0x7c: OOM_G (last partial freed < 2 pages), OOM_G2 (the GBL case),
                     # OOM_GRANTED (a grant already given this scene). Survive the SC_OOM clear; reset at TA
                     # finish (the DDK's 0x785|0x18 mask).
OOM_G = 0x1
OOM_G2 = 0x2
OOM_GRANTED = 0x4
OOM_MARK_TA = 0x21   # control block +0x84: (0x724>>16) at the last OOM minus 2, 16-bit (count<2 wraps to
                     # -2/-1 signed, harmless). The teardown sets OOM_G when the post-render count > this.
OOM_MARK_GL = 0x22   # control block +0x88: max((0x72c>>16)-2, 0) at the last OOM. The GBL equivalent.
OOM_SAVED_624 = 0x23 # control block +0x8c: the ZLS threshold (0x624) saved before a grant adds 32; restored
                     # at TA finish.
STAGE1_FLAG = 0x24   # control block +0x90: the DDK's stage-1 state (sa8 ~b19). Set by a stage-1 RAISE (move the
                     # global threshold to the current 0x72c count + reload the TA free list, no render), cleared
                     # by the stage-1 UNDO at the next OOM. The DDK alternates OOM->stage-1-raise / OOM->undo+
                     # partial-render; its reloads ride with the raise/undo (a bare reload corrupts the ring).
                     # Carries ACROSS scenes -- not cleared at TA finish; only init_set clears it.
STAGE1_SAVED_628 = 0x25 # control block +0x94: the global threshold (0x628) saved before a stage-1 raise; the
                     # stage-1 undo restores it. Separate from OOM_SAVED_628 (the +16 grant's save).
SLOT1_INIT = 0x26    # control block +0x98: set once slot 1 has a valid DPM CONTROL state. The partial render's
                     # 3D frees run through slot 1's control (0x698/0x634); we never run a real scene in slot 1
                     # (the DDK does, ctx630=4/5/7), so its control is stale and the frees link pages off the
                     # TA's chain -> leak (count recovers, ring short). Before the first
                     # shuffle, give slot 1 a control state like a DDK first kick would. Cleared in init_set.
SC_OOM = 0x17        # control block +0x5c: OOM partial-render state. 0 = none; 1 = OOM taken, the
                     # TA is paused (0xa74 b0) and held (0x69c=4), awaiting TA_TERMINATE; 2 = the
                     # partial render is in flight (its 3D end resumes the TA via 0x69c=1, unpauses,
                     # clears the DPM control, and skips the scene teardown). Cleared by init_set.
SC_INFLIGHT = 0x14   # control block +0x50: the RT-data entry of the render in
                     # flight (0 = none). The DDK keeps a doubly-linked render-
                     # in-flight list; one context needs only this single slot,
                     # at the DDK's own offset so the A/B reference still matches.
SC_XFER = 0x15       # control block +0x54: the transfer context in flight (0 =
                     # none). Lets the 3D-end handler tell a transfer's completion
                     # from a render's. (the microkernel.s own slot; a transfer and a
                     # render never overlap on one context.)
SC_TAFLIGHT = 0x36   # control block +0xd8: 1 while a TA is in flight. Serialises
                     # host kicks. Must be a word srvinit never writes: +0x64
                     # (word 0x19) is clobbered by srvinit's control-block setup
                     # (k_ta then deferred the very FIRST kick, do_ta never
                     # ran, all markers 0). 0x36 is in the microkernel's private
                     # region (0 on every prior dump), next to the sentinel words.
                     # host kicks: a TA kick arriving while this is set is left in
                     # the kernel CCB and re-launched from ta_finished (the DDK
                     # pattern). Without it a fast host delivers kick 2
                     # while TA 1 runs, re-kicking the TA mid-run (lockup_TA) and
                     # making ta_finished act on the wrong command (a race).
PB_TA_LOADED = 0x2e  # control block +0xb8 (the DDK's loaded-PB marker, ta3d+0xb8): the PB
                     # descriptor whose TA free list is currently loaded (0 = none). do_ta
                     # loads the TA free list ONLY when the kick's PB differs -- the DDK loads
                     # once per PB and lets the hardware keep the ring in sync.
                     # Reloading every frame re-seeds the head from pb+0x18 (last
                     # frame's snapshot), skipping the ~25 pages the 3D freed -> DPM_OUT_OF_MEM
                     # at ~frame 40 (the M1 exit). Reset at init (post-HWR the HW DPM is
                     # wiped) and at cleanup (so a same-address PB realloc reloads).
PB_3D_LOADED = 0x2f  # control block +0xbc (the DDK's ta3d+0xbc): the PB whose 3D free list +
                     # save_ta_state are loaded. do_3d gates those on it the same way; 0x63c=3
                     # and the ISP/PBE state (D3_RD) still run every 3D start.
SC_VERSION = 0x18    # control block +0x60: microkernel version, written
                     # at init. Advisory (the DDK driver does not read it); a
                     # version-aware loader logs it (docs/UKERNEL-DECISIONS.md).
                     # A free slot: it was 0x16 before, which is the kernel CCB
                     # base — init overwrote that base with the version word.
UK_VERSION = 0x00010000   # 1.0 (packed major<<16 | minor)
# The SGX core this image targets. The microkernel programs core registers for a
# specific SGX5xx variant (register-programming tables = docs/UKERNEL.md §7), so
# another core (e.g. SGX540/SGX531) needs its own image built from a core-specific
# source. build.py names the output and the manifest "core" field from this, so the
# images never collide. SGX545 is the CE5300's GPU.
CORE_NAME = "sgx545"
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
RT_RENDER = 0x4 // 4         # RT-data entry: the render using this RT (0 = free)
RT_DATA = 0x30 // 4          # RT-data entry word holding the RT-data pointer
RT_STATE = 0x34 // 4         # RT-data entry state flag
RT_IDLE_STATE = 0x10         # the value that flag holds when the RT is idle
RD_STATE = 0x2A4             # render-details state: set to 2 (freed) at completion
CTX_INFLIGHT0 = 0x28 // 4    # HW context: the render in flight (two words + a flag)
CTX_INFLIGHT1 = 0x2C // 4
CTX_INFLIGHT_FLAG = 0x3C // 4

# ---- host control block (SGXMKIF_HOST_CTL) word offsets --------------------
HC_INIT = 0
HC_POWER = 1         # ui32PowerStatus
HC_CLEANUP = 2       # ui32CleanupStatus
HC_INTFLAGS = 8      # ui32InterruptFlags (microkernel -> host); bit 1 = ACTIVE_POWER
HC_INTCLEAR = 9      # ui32InterruptClearFlags; bit 0 = INTERRUPT_HWR
HC_TIMERCLK = 6      # ui32uKernelTimerClock (host-set EVENT_TIMER period)
HC_BGBLOCK = 15      # word +0x3c: bit 0 = HOSTCTL_HOST_BGBLOCK -- the host wrote the
                     # background block on the CPU, so do_3d skips its EDM pulse (§4a).
                     # This word is the DDK's ui32PerfGroup (HWPerf counter group); reusing it
                     # is safe because powervr_sgx has no HWPerf and the driver sets it only for
                     # open images (never the DDK one), and our uK never reads it as PerfGroup.
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
CMD_CACHECTL = 1     # ui32CacheControl (SGXMKIF_CC_* bits the host owes since its last command)
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
CR_EVENT_TIMER = 0xACC       # EUR_CR_EVENT_TIMER: bit 24 = ENABLE, low bits = period
                             # (host ui32uKernelTimerClock). A periodic timer task
                             # keeps the EDM moving so the host watchdog does not
                             # false-HWR a quiescent core between kicks.
EVENT_TIMER_ENABLE = 0x01000000
EVENT_TIMER = 0x20000000     # EUR_CR_EVENT_STATUS bit 29: the timer event
CR_USE_INVAL = 0xA08         # USE cache invalidate
CR_PDS_INV0 = 0xAD0          # PDS cache invalidate, TA side
CR_PDS_INV1 = 0xAD4          # PDS cache invalidate, 3D side
CR_PDS_INV_CSC = 0xAE0       # coefficient-store invalidate
CR_DPM_STATE = 0x694
CR_DPM_CONTROL = 0x698
CR_DPM_FL_MODE = 0x664       # DPM free-list load selector (TA = 3, 3D = 5)
CR_DPM_TA_FL = 0x684         # DPM TA free-list load  -> EUR_CR_EVENT_STATUS2 bit 0
CR_DPM_3D_FL = 0x680         # DPM 3D free-list load  -> EUR_CR_EVENT_STATUS2 bit 1
CR_DPM_OOM = 0x69c           # DPM out-of-memory allocation control (4 = hold the TA's
                             # allocation during an OOM partial render, 1 = release/resume it). Not
                             # in the GPL header; HW iteration may refine. The partial-render start
                             # writes 4, its completion writes 1.
CR_DPM_LOADSEL = 0x65c       # DPM load-select gate around the slot store/load in the OOM shuffle:
                             # 1 to open the store/load, 0 to close.
CR_OTPM = 0x24C
CR_TPC = 0x224
CR_EVENT_HOST_CLEAR_0x140 = 0x140    # the invalidate-done acknowledge register
INV_DONE_TA = 0x27           # 0x138 bits for the TA-side invalidate
INV_DONE_3D = 0x4B           # 0x138 bits for the 3D-side invalidate
SW_EVENT = 0x4000
DCU_INVALID = 0x400          # EUR_CR_EVENT_STATUS2: a data-cache-unit invalidate
                             # completion. Pending after the kernel's SysFinalise;
                             # the event program must acknowledge it
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
# blit has first tile 0, a sub-rectangle blit does not (confirmed against a
# capture). Word 4 (depth/stencil block) is 0 in every capture.
# fixed transfer-start constants (the render's 0x63c=3 is 0 for a transfer; no PB)
T3_CONST = [(0x480, 0), (0x63c, 0), (0x43c, 1)]
TRANSFERCMD_SHARED = 0xac     # ui32SharedCmdCCBOffset points here; command base is 0xac before

# ---- per-core handler features (docs/porting/cores/) ------------------------
# Each flag selects a path in ukernel.py's handlers; these are the SGX545 values,
# and another core overrides only what differs. The defaults emit the code the
# SGX545 always had, so its image is unchanged.
MULTI_MEM_CONTEXTS = True    # BIF directory lists 1-15 + BANK0 routing. False: one
                             # list (DIR_LIST_BASE0), switched per job (sgx530.py)
MULTI_EVENT_KICK = True      # host side only: the host's init kick uses EVENT_KICK2.
                             # The event entry tells init from a kick by
                             # ui32InitStatus, so the microkernel is the same either way
FIX_HW_BRN_28889 = False     # never issue a BIF and a data-cache invalidate together
WAIT_BOUND = 0               # 0: a wait() spins until its bits set. N: give up after N
                             # polls (16-bit), log it (WAIT_TIMEOUT_*) and go on
WAIT_TIMEOUT_ID = 0x28       # control block +0xa0: the last wait that timed out
WAIT_TIMEOUT_CT = 0x29       # control block +0xa4: how many waits timed out
HEARTBEAT = None             # a control block word the event entry increments, for
                             # a host watchdog that can't read EDM task registers
PD_TABLE_SLOTS = 1024        # one-context cores: the host's read-only PD table (a 4 KiB
                             # page of DIR_LIST_BASE0 values; slot 0 = the firmware PD)

INIT_DPM_CLEAR = [CR_DPM_LOADSEL, 0x630]  # init zeroes the DPM load-select and slot
                             # select, so an HWR leaves no stale OOM-shuffle state

ISA_CORE = 545               # the instruction encodings: sgx-asm -c sgx<ISA_CORE>
DIAG_ENTRY = None            # a bring-up marker: the event entry stores markers before
                             # phas.i (cores/sgx530_diag.py); None = the normal entry
DIAG_PHAS = True             # False drops the event entry's phas.i (diagnostic builds only)
PROBES = None                # code-base-unit probes past the image (cores/sgx530_probe.py)
BASE_PADS = None             # code-base pads past the image (cores/sgx530_bases.py)
USSE_PROBE = None            # a minimal self-looping event task (cores/sgx530_usse.py)
USSE_PHAS = False            # USSE probe: emit phas.i first (cores/sgx530_usse_phas.py)
USSE_MIRROR = False          # USSE probe: mirror the real entry preamble (cores/sgx530_usse_mirror.py)
USSE_MOE = True              # USSE mirror probe: False drops [moe] (cores/sgx530_usse_nomoe.py)
USSE_LDGATE = False          # isolate one ld.a.d, str-gated (cores/sgx530_usse_ld*.py)
USSE_LDOFF = 0x2a            # the ld.a.d offset for USSE_LDGATE (default HEARTBEAT)
USSE_LDBASE = None           # None = r17 (ctlblk); an int = limm it into r5 as the base
USSE_LDREG = None            # ld-gate: name register rN as the base, value unused (reg sweep)
USSE_LDPRELOAD = False       # ld-gate: preload r0..r22 = (N+1)<<16 first (which-register probe)
USSE_LIMMTEST = None         # limm rK then str rK to EVENT_TIMER: does limm write? (cores/sgx530_limmwr_r*.py)
USSE_ALUWRITE = False        # or r5,r5,r17 then ld r5: does a non-limm ALU write land? (cores/sgx530_aluwr.py)
USSE_PHAS_TEMPS4 = 0         # phas.i temps4 field (temporaries/4); 0 = the normal phas.i #0,#0
USSE_ALU_NOPS = 0            # aluwr: nops between the or and the ld (pairing/latency test)
USSE_ALU_DST = 5             # aluwr: the register the or writes and the ld reads back
USSE_PA_READ = False         # read pa0 as the ld base: are PDS-set attributes visible? (cores/sgx530_pa0.py)
USSE_ALU_DSTOP = None        # aluwr: an explicit dest/base operand (e.g. 'o0','pa0') for the write-file test

# ---- clock gating, reset, BIF (EUR_CR byte offsets and fields) --------------
CR_CLKGATECTL = 0x000
CR_CLKGATESTATUS = 0x008
CLKGATE_ISP_MASK = 0x3       # CLKGATECTL's ISP field (b0-1)
CLKGATE_ISP_AUTO = 0x2       # ... set to clock-on-demand
CLKGATESTATUS_ISP = 0x1      # CLKGATESTATUS's ISP bit (b0)
CR_SOFT_RESET = 0x080
SOFT_RESET_TA = 0x4          # SOFT_RESET b2
CR_BIF_CTRL = 0xC00
BIF_CTRL_FLUSH = 0x4
BIF_CTRL_INVALDC = 0x8
CR_BIF_DIR_LIST_BASE0 = 0xC84
CR_BIF_DIR_LIST_BASE1 = 0xC38
CR_BIF_BANK0 = 0xC78
CR_BIF_MEM_REQ_STAT = 0xCA8
BIF_MEM_REQ_MASK = 0x7ff     # reads b0-10
# the waits after the OTPM invalidate + clear in dpm_load: (poll reg, bits, ack reg)
OTPM_CLEAR_WAITS = [(CR_EVENT_STATUS, 0x100000, CR_EVENT_HOST_CLEAR),
                    (CR_EVENT_STATUS2, 0x1000, CR_EVENT_HOST_CLEAR2)]
FEATURES = 2                 # default PVRSGX_UKFW_* bits in the manifest (UK_FEATURES
                             # overrides): SECURE, the host writes the background block,
                             # so powervr_sgx needn't map its heaps into client PDs
