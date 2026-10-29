"""The SGX545's event data master around emu.py: kernel data laid out as
srvinit lays it out (docs/SRVINIT.md), the five microkernel PDS programs as
behaviour, and helpers that play the host's part (init, CCB kicks, events).

    m = Machine(open("ukernel.bin", "rb").read())
    m.init()                         # SGXInitialise: scripts, kick, init handshake
    m.kick(cmd_type, data=[...])     # SGXScheduleCCBCommand + EVENT_KICK
    m.event(in0, in1, in2)           # a hardware event, as the EDM delivers it

The PDS programs are modelled from their description in docs/SRVINIT.md, not
run: pds1 DMAs the TA3D control block into SA0-63 (unless in0 is set) and
starts the microkernel's secondary entry; pds2 starts the init entry; pds3
dispatches events; pds4 dispatches the microkernel's own deferred tasks.
`Machine(..., entries=...)` takes another microkernel's entry offsets.
"""
import struct

from emu import USSE, Mem, Regs, M32

KCODE = 0x30000000
KDATA = 0x32000000

# srvinit's allocations (docs/SRVINIT.md, "Allocations"), at the addresses the
# EA3 gave
A = {
    "kccb": 0x32000000, "ccbctl": 0x32002000, "kicker": 0x32002010,
    "ta3d": 0x32003000, "hostctl": 0x32003100, "misc": 0x32003160,
    "hwperf": 0x32004000, "tasig": 0x3200a420, "sig3d": 0x3200b000,
    "ukernel": 0x30000000, "pds0": 0x3000cf70, "pds1": 0x3000cf90,
    "pds2": 0x3000cfd0, "pds3": 0x3000d000, "pds4": 0x3000d100,
}
PD_PHYS = 0x1F000000          # any value: the page directory srvinit is given

# the 1.7.862890 microkernel's entry points, byte offsets (docs/SRVINIT.md)
DDK_ENTRIES = {
    "pds0": 0xD60, "pds1": 0xDF0, "pds2": 0xE20,
    "ev_default": 0x30, "ev_a": 0x7050, "ev_b": 0x2CD0, "ev_c": 0x5670,
    "def_a": 0x73A0, "def_b": 0x2EA0, "def_c": 0x5740,
    "kick": [0x11D, 0x103, 0, 0xD4, 0xEB, 0x13A, 0x173, 0x156, 0, 0x157, 0],
    "patch": True,
}
TASIG = [0x304, 0x26C, 0x270, 0x2A0, 0x2A4, 0x378]
SIG3D = ([0x438, 0x4CC, 0x4D0, 0x4D4, 0x4D8] + list(range(0x510, 0x520, 4)) +
         list(range(0x524, 0x548, 4)) + list(range(0x550, 0x560, 4)) +
         list(range(0x84C, 0x85C, 4)) + list(range(0x560, 0x570, 4)) + [0x308, 0x30C])


def limm_patch(code, off, v):
    lo, hi = struct.unpack_from("<II", code, off)
    assert (lo & 0x1FFFFF) == 0xDBEEF and hi == 0xFC237150, f"no placeholder at {off:#x}"
    lo = (lo & 0xFFE00000) | (v & 0x1FFFFF)
    hi = (hi & 0xFFFC0E0F) | ((v >> 17) & 0x1F0) | ((v >> 26) << 12)
    struct.pack_into("<II", code, off, lo, hi)


class Machine:
    def __init__(self, fw, entries=DDK_ENTRIES, hwperf=0, mem=None):
        self.E = entries
        code = bytearray(fw)
        if entries.get("patch"):
            for off, v in ((0xBAF8, 0x1800100), (0xBB10, A["pds4"] >> 4), (0xD60, 0x1001000),
                           (0xD70, A["pds1"] >> 4), (0xD78, A["ta3d"]), (0xDB0, 0x800100),
                           (0xDC0, A["pds2"] >> 4), (0xE40, A["pds3"] & ~0xF), (0xE50, 6)):
                limm_patch(code, off, v)
        self.code = bytes(code)
        self.mem = mem or Mem()
        self.regs = Regs()
        self.u = USSE(self.code, KCODE, self.mem, self.regs)
        self.u.on_unknown = self._unknown
        self.queue = []           # PDS tasks waiting: (pds address, in0)
        self.log = []             # (kind, ...) machine-level events
        m = self.mem
        m.load(A["ukernel"], self.code)
        t = A["ta3d"]
        m.wr(t + 0x00, t)
        m.wr(t + 0x04, A["hostctl"])
        m.wr(t + 0x08, A["ccbctl"])
        m.wr(t + 0x4C, hwperf)
        m.wr(t + 0x58, A["kccb"])
        m.wr(t + 0x5C, PD_PHYS)
        m.wr(t + 0xAC, A["hwperf"])
        m.wr(t + 0xB0, A["tasig"])
        m.wr(t + 0xB4, A["sig3d"])
        m.wr(A["tasig"], len(TASIG))
        for i, r in enumerate(TASIG):
            m.wr(A["tasig"] + 8 + 4 * i, r)
        m.wr(A["sig3d"], len(SIG3D))
        for i, r in enumerate(SIG3D):
            m.wr(A["sig3d"] + 8 + 4 * i, r)
        self.ccb_w = 0
        self.hw_model()

    # a minimal model of the hardware's answers to polled registers --------
    def hw_model(self):
        install_hw_model(self.regs)

    def auto_polls(self, regs=(0x118, 0x12C, 0x138), after=4):
        """exploration aid: a register read `after` times in a row from one pc
        gets the bits the following test waits for (logged as 'auto')"""
        import re
        last = {"k": None, "n": 0}
        u = self.u

        def hook(a):
            def f(R, v):
                pc = R.log[-1][3] if R.log else None
                k = (a, u_pc[0])
                last["n"] = last["n"] + 1 if last["k"] == k else 1
                last["k"] = k
                if last["n"] >= after:
                    for q in range(u_pc[0] + 1, u_pc[0] + 5):
                        t = u.inst(q)[1]
                        m = re.match(r"test\.and\.(eq|ne) p\d, r\d+, #(\d+)", t)
                        if m:
                            bits = int(m.group(2))
                            self.log.append(("auto", a, bits, u_pc[0]))
                            last["n"] = 0
                            return v | bits
                return None
            return f
        u_pc = [0]
        old_step = u.step

        def step(t):
            u_pc[0] = t.pc
            return old_step(t)
        u.step = step
        for a in regs:
            prev = self.regs.on_read.get(a)
            self.regs.on_read[a] = hook(a) if prev is None else prev

    # ------------------------------------------------------------------
    def _unknown(self, u, t, x, text):
        n = x.form.name
        if n in ("op0xff", "op0x100"):
            self.log.append(("op", t.pc - 1, text, [u.rd(t, o) for o in x.ops]))
            return True
        return False

    def entry(self, name):
        return self.E[name] // 8

    def run_task(self, pc, pa=()):
        self.log.append(("task", pc, list(pa)))
        t = self.u.task(pc, pa)
        self.drain_emits()
        return t

    def drain_emits(self):
        while self.u.emits:
            pc, tgt, vals, d = self.u.emits.pop(0)
            if tgt != 2:
                self.log.append(("emit", pc, tgt, vals))
                continue
            ctrl, addr4, in0 = vals[0], vals[1], vals[2]
            self.log.append(("pds", pc, ctrl, addr4 << 4, in0))
            self.queue.append((addr4 << 4, in0, ctrl))

    def run_queue(self, limit=100):
        n = 0
        while self.queue and n < limit:
            addr, in0, ctrl = self.queue.pop(0)
            self.pds(addr, [in0, 0, 0, 0, 0])
            n += 1

    def pds(self, addr, ins):
        """run one of the five microkernel PDS programs, as behaviour"""
        E = self.E
        if addr == A["pds0"]:
            self.run_task(self.entry("pds0"))
        elif addr == A["pds1"]:
            if ins[0] == 0:
                for i in range(64):
                    self.u.sa[i] = self.mem.rd(A["ta3d"] + 4 * i)
            self.run_task(self.entry("pds1"))
        elif addr == A["pds2"]:
            self.run_task(self.entry("pds2"))
        elif addr == A["pds3"]:
            self.event(*ins[:5])
        elif addr == A["pds4"]:
            in0, in4 = ins[0], ins[4]
            if in0 & 0x4000F:
                e = "def_a"
            elif in0 & 0x120080:
                e = "def_b"
            else:
                e = "def_c"
            self.run_task(self.entry(e), [in0, 0, in4])
        else:
            raise RuntimeError(f"PDS program at {addr:#x}?")

    def event(self, in0=0, in1=0, in2=0, in3=0, in4=0):
        """pds3, the event program: in0/in2/in4 -> PA0-PA2, dispatch on in1"""
        t = in1 & 0xFFFFFF
        e = "ev_default"
        if not t & 0x100 and in0 == 0:
            for m, name in ((0x400090, "ev_a"), (0x200, "ev_b"), (0x300400, "ev_c")):
                if t & m:
                    if not t & ~m & 0xFFFFFF:
                        e = name
                    break
        self.log.append(("event", in0, in1, in2, in4, e))
        self.run_task(self.entry(e), [in0, in2, in4])
        self.run_queue()

    # host side ------------------------------------------------------------
    def init(self):
        """SGXInitialise after the scripts: zero InitStatus, kick, run the event
        program the scripts installed (pds0), poll InitStatus"""
        h = A["hostctl"]
        self.mem.wr(h, 0)
        self.regs.v[0xA68] = A["pds0"]
        self.pds(A["pds0"], [0] * 5)
        self.run_queue()
        return self.mem.rd(h)

    def kick(self, cmd, data=(), cache=0, events=None):
        """write a kernel CCB command and raise the host-kick event"""
        k = A["kccb"] + 32 * self.ccb_w
        self.mem.wr(k, self.E["kick"][cmd])
        self.mem.wr(k + 4, cache)
        for i in range(6):
            self.mem.wr(k + 8 + 4 * i, data[i] if i < len(data) else 0)
        self.ccb_w = (self.ccb_w + 1) & 255
        self.mem.wr(A["ccbctl"], self.ccb_w)
        self.event(*(events or (0, 0x100, 0x100)))

    def hostctl(self):
        return self.mem.words(A["hostctl"], 17)



def install_hw_model(r):
    """install the register model a working GPU presents to the microkernel:
    the done bits each poll loop waits for appear when the request is written
    (docs/UKERNEL.md, "Register sequences"). Shared by the DDK machine and
    ukernel/run_ours.py."""
    r.v[0x138] = 0

    # cache invalidates report completion in 0x138, one bit group per unit:
    # PDS_INV0 -> 0x4, PDS_INV1 -> 0x8, PDS_INV_CSC -> 0x3, 0xa08 (USE) -> 0x60.
    # The TA side waits for 0x27, the 3D side for 0x4b.
    def inval(bits):
        def f(regs, v):
            if v & 1:
                regs.v[0x138] = regs.v.get(0x138, 0) | bits
        return f
    for a, bits in ((0xAD0, 0x4), (0xAD4, 0x8), (0xAE0, 0x3), (0xA08, 0x60)):
        r.on_write[a] = inval(bits)

    # 0x140 (written with the bits just polled) is modelled as no-op: the
    # microkernel waits for 0x20 again after one 0xa08 and two acks, so
    # the done bits behave as levels here (docs/UKERNEL.md, open questions)

    def cr804(regs, v):             # 0x804 bit 28: MADD cache invalidate -> EVENT_STATUS bit 26
        if v & 0x10000000:
            regs.v[0x12C] = regs.v.get(0x12C, 0) | 0x04000000
        return v & ~0x10000000
    r.on_write[0x804] = cr804

    def host_clear(regs, v):        # EVENT_HOST_CLEAR clears EVENT_STATUS bits
        regs.v[0x12C] = regs.v.get(0x12C, 0) & ~v
    r.on_write[0x134] = host_clear

    def dpm_load(bit):              # 0x684 / 0x680 = 1: DPM TA / 3D free-list load -> EVENT_STATUS2 bit 0 / 1
        def f(regs, v):
            if v & 1:
                regs.v[0x118] = regs.v.get(0x118, 0) | bit
        return f
    r.on_write[0x684] = dpm_load(1)
    r.on_write[0x680] = dpm_load(2)

    def dpm_op(store, clear, load):
        # 0x694 / 0x698 (DPM state / control): bits 0-2 = store / clear / load,
        # address >> 3 above; each reports its own EVENT_STATUS bit
        def f(regs, v):
            b = 0
            if v & 1:
                b |= store
            if v & 2:
                b |= clear
            if v & 4:
                b |= load
            regs.v[0x12C] = regs.v.get(0x12C, 0) | b
        return f
    r.on_write[0x694] = dpm_op(0x10, 0x40, 0x20)
    r.on_write[0x698] = dpm_op(0x80, 0x200, 0x100)

    def otpm(regs, v):              # 0x24c = 9: OTPM invalidate + clear -> STATUS bit 20, STATUS2 bit 12
        if v & 1:
            regs.v[0x12C] = regs.v.get(0x12C, 0) | 0x100000
        if v & 8:
            regs.v[0x118] = regs.v.get(0x118, 0) | 0x1000
    r.on_write[0x24C] = otpm

    def tpc(regs, v):               # 0x224 bit 30: TPC flush -> EVENT_STATUS bit 10
        if v & 0x40000000:
            regs.v[0x12C] = regs.v.get(0x12C, 0) | 0x400
    r.on_write[0x224] = tpc

    def host_clear2(regs, v):
        regs.v[0x118] = regs.v.get(0x118, 0) & ~v
    r.on_write[0x114] = host_clear2
