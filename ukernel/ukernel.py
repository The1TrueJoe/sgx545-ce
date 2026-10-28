"""openHC's own SGX545 USSE microkernel, in assembly.

This is the open replacement for Imagination's EDM microkernel. It is written
from the behavioural specification in `docs/UKERNEL.md`, not translated from
the DDK's bytes: it reproduces what the DDK microkernel *does* (the event
dispatch, the CCB host-kick handlers, the init handshake, the completion and
sync updates, the TA/3D register sequences) in our own code and our own
layout.

`build.py` turns this into the binary `srvinit` loads and a manifest of the
entry offsets and immediate-patch sites srvinit needs (the open equivalent of
the DDK build's fixed constants in `docs/SRVINIT.md`). Everything is checked
in the offline emulator (`tools/ukernel/`) before any hardware run.

The program is one flat USSE image. Entry points are labels; `build.py`
resolves them to instruction offsets and records them in the manifest. Each
entry is a USSE task the event / PDS machinery starts (docs/UKERNEL.md §1):
it runs to a program-end instruction and stops.

State between tasks lives in the TA3D control block, DMAed to the secondary
attributes by the entry PDS program, exactly as the DDK's does (§1). The
field offsets are the openHC layout and match what srvinit writes
(docs/SRVINIT.md "TA3D control"):

  sa0  = control block's own address        sa1  = host control block address
  sa2  = CCB control (write/read offsets)    sa16 = kernel CCB base

The host control block (sa1) is SGXMKIF_HOST_CTL; word 0 is ui32InitStatus.
"""

# control-block field word indices (as secondary attributes)
SA_SELF = 0
SA_HOSTCTL = 1
SA_CCBCTL = 2
SA_CCB = 16

# host control block word offsets (SGXMKIF_HOST_CTL, this build: no breakpoints)
HC_INIT = 0            # ui32InitStatus
HC_POWER = 1           # ui32PowerStatus
HC_CLEANUP = 2         # ui32CleanupStatus

# the completion bits the host polls (sgx_mkif_km.h)
EDM_INIT_COMPLETE = 1

# EUR_CR registers (word index = byte offset / 4), the ones this first version uses
CR_EVENT_STATUS = 0x12C // 4
CR_EVENT_HOST_CLEAR = 0x134 // 4
SW_EVENT = 0x4000


class Asm:
    """collects (label, text) lines; labels become instruction offsets"""

    def __init__(self):
        self.lines = []
        self.labels = {}

    def label(self, name):
        self.labels[name] = None
        self.lines.append(("label", name))

    def i(self, text):
        self.lines.append(("i", text))

    # small helpers for readability
    def end(self):
        self.i("nop [end]")

    def phas(self):
        self.i("phas.i #0, #0 [wait=7]")


def build(a):
    """emit the microkernel; returns the Asm with labels marking entries"""

    # ---- init entry -------------------------------------------------------
    # Reached once at boot, after the control block is in sa (docs/UKERNEL.md
    # §4). Set ui32InitStatus = PVRSRV_USSE_EDM_INIT_COMPLETE so the host's
    # PollForValueKM succeeds, then end. The host control block address is sa1.
    a.label("init")
    a.phas()
    a.i(f"limm r0, #{EDM_INIT_COMPLETE}")
    a.i(f"st.a.d sa{SA_HOSTCTL}, #{HC_INIT}, r0 [moe]")
    a.i("idf")
    a.i("wdf")
    a.end()

    # ---- raise the host interrupt (a shared tail) -------------------------
    # The microkernel signals the host by setting SW_EVENT in EVENT_STATUS
    # (docs/UKERNEL.md §2/§5). Called with the link register set; returns.
    a.label("raise_host_irq")
    a.i(f"limm r0, #{SW_EVENT}")
    a.i(f"str #{CR_EVENT_STATUS}, r0")
    a.i("lapc")

    return a


def program():
    a = Asm()
    return build(a)
