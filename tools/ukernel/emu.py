"""A USSE emulator for microkernel-style code (docs/UKERNEL.md, "The emulator").

It runs the scalar integer subset the SGX545 microkernel uses: moves, limm,
bitwise, test, the integer multiply-adds, loads and stores to device memory,
register-bank access (ldr/str), branches and calls (ba/br/lapc/savl/setl),
emit (task launch) and the task-ending forms. Instructions are decoded with
tools/isa/hw.py, so anything the decoder knows can be traced; anything the
emulator has no semantics for stops the run with an error rather than being
guessed at.

The machine around it is a model, not the hardware:
  Mem    sparse 32-bit device-virtual memory (word granular)
  Regs   the EUR_CR register file, with optional per-register hooks so a test
         can model hardware side effects (status bits, kicks)
  USSE   one task at a time: r/pa/o/i/idx/predicates per task, sa persistent
         (the EDM's secondary attributes survive between tasks)

    emu = USSE(code, base=0x30000000, mem=Mem(), regs=Regs())
    emu.task(pc, pa=[...])          # run one task to its end
    emu.regs.log                    # [(op, addr, value, pc)] register traffic
"""
import os
import re
import struct
import sys

# appended, not inserted: tools/isa/dis.py would shadow the standard library's dis
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "isa"))
import hw  # noqa: E402

M32 = 0xFFFFFFFF


def rol32(v, n):
    n &= 31
    return ((v << n) | (v >> (32 - n))) & M32 if n else v & M32


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


class EmuError(Exception):
    pass


class Mem:
    """sparse device memory; unwritten words read as `fill`"""

    def __init__(self, fill=0):
        self.w = {}
        self.fill = fill
        self.log = None          # list to record (op, addr, value, pc)

    def rd(self, a, pc=None):
        if a & 3:
            raise EmuError(f"unaligned read {a:#x}")
        v = self.w.get(a & M32, self.fill)
        if self.log is not None:
            self.log.append(("ld", a, v, pc))
        return v

    def wr(self, a, v, pc=None):
        if a & 3:
            raise EmuError(f"unaligned write {a:#x}")
        self.w[a & M32] = v & M32
        if self.log is not None:
            self.log.append(("st", a, v & M32, pc))

    def load(self, a, data):
        for i in range(0, len(data) - 3, 4):
            self.w[a + i] = struct.unpack_from("<I", data, i)[0]

    def words(self, a, n):
        return [self.w.get(a + 4 * i, self.fill) for i in range(n)]


class Regs:
    """the EUR_CR register file (byte offsets). on_write / on_read hooks:
    {offset: f(regs, value) -> stored value | None}"""

    def __init__(self):
        self.v = {}
        self.log = []
        self.on_write = {}
        self.on_read = {}

    def rd(self, a, pc=None):
        if a in self.on_read:
            r = self.on_read[a](self, self.v.get(a, 0))
            if r is not None:
                self.v[a] = r & M32
        v = self.v.get(a, 0)
        self.log.append(("ldr", a, v, pc))
        return v

    def wr(self, a, v, pc=None):
        v &= M32
        self.log.append(("str", a, v, pc))
        if a in self.on_write:
            r = self.on_write[a](self, v)
            if r is not None:
                v = r & M32
        self.v[a] = v


# hardware constants c0-c63 (docs/ISA.md "Hardware constants"), integer view
HWC = [0, 0, 0, 0x3F800000] + [0] * 60
for _i in range(4, 14):
    HWC[_i] = struct.unpack("<I", struct.pack("<f", float(2 ** (_i - 3))))[0]
for _i in range(14, 28):
    HWC[_i] = struct.unpack("<I", struct.pack("<f", 2.0 ** -(_i - 13)))[0]
for _i in range(48, 52):
    HWC[_i] = 0
for _i in range(52, 56):
    HWC[_i] = 0x3F800000
for _i in range(56, 60):
    HWC[_i] = M32
for _i in range(60, 64):
    HWC[_i] = 0x7FFF7FFF

PRED_RE = re.compile(r"^\((!?)p(\d)\) ")


class Task:
    def __init__(self, pc, pa, poison):
        self.r = [poison + i for i in range(128)]
        self.pa = list(pa) + [0] * (128 - len(pa))
        self.o = [0] * 128
        self.i = [0] * 4
        self.idx = [0, 0, 0]
        self.p = [False] * 4
        self.link = None
        self.pc = pc


class Ended(Exception):
    pass


class USSE:
    def __init__(self, code, base=0x30000000, mem=None, regs=None):
        self.code = code
        self.base = base
        self.n = len(code) // 8
        self.dec = [None] * self.n
        self.mem = mem or Mem()
        self.regs = regs or Regs()
        self.sa = [0] * 128
        self.g = [0] * 64          # special registers (g<n>); a test sets what it needs
        self.emits = []            # (pc, target, values, flags)
        self.trace = None          # list of (pc, text) when tracing
        self.trace_max = 1 << 20
        self.on_unknown = None     # f(emu, task, inst, text) -> True if handled
        self.steps = 0
        self.bp = {}               # pc -> f(emu, task), called before the instruction
        self.lock_halt = False

    # ------------------------------------------------------------------
    def inst(self, pc):
        if not 0 <= pc < self.n:
            raise EmuError(f"pc {pc:#x} outside the program")
        d = self.dec[pc]
        if d is None:
            lo, hi = struct.unpack_from("<II", self.code, pc * 8)
            x = hw.decode(lo, hi)
            if x is None:
                raise EmuError(f"{pc:#06x}: undecodable {hi:08x} {lo:08x}")
            t = hw.fmt(x)
            m = PRED_RE.match(t)
            pred = None
            if m:
                pred = (m.group(1) == "!", int(m.group(2)))
            elif t.startswith("(pn) "):
                pred = "pn"
            d = self.dec[pc] = (x, t, pred)
        return d

    # operand access -------------------------------------------------
    def bank(self, t, k):
        if k == "r":
            return t.r
        if k == "pa":
            return t.pa
        if k == "sa":
            return self.sa
        if k == "o":
            return t.o
        if k == "i":
            return t.i
        raise EmuError(f"bank {k}")

    def rd(self, t, o, k=0):
        kind = o["kind"]
        if kind == "#":
            return o["n"] & M32
        if kind == "idx":
            return t.idx[o["n"]]
        if kind == "ix":
            return self.bank(t, o["bank"])[(t.idx[o["idx"]] + o["off"] + k) & 127]
        if kind == "c":
            return HWC[o["n"]]
        if kind == "g":
            return self.g[o["n"]]
        if kind == "p":
            return int(t.p[o["n"]])
        return self.bank(t, kind)[(o["n"] + k) & 127]

    def wr(self, t, o, v, k=0):
        kind = o["kind"]
        v &= M32
        if kind == "idx":
            t.idx[o["n"]] = v
        elif kind == "ix":
            self.bank(t, o["bank"])[(t.idx[o["idx"]] + o["off"] + k) & 127] = v
        elif kind == "g":
            self.g[o["n"]] = v
        else:
            self.bank(t, kind)[(o["n"] + k) & 127] = v

    # ------------------------------------------------------------------
    def task(self, pc, pa=(), poison=0xBAD00000, max_steps=200000):
        """run one task from instruction `pc` until it ends; returns the Task"""
        t = Task(pc, pa, poison)
        try:
            for _ in range(max_steps):
                self.step(t)
        except Ended:
            return t
        raise EmuError(f"task from {pc:#x} did not end in {max_steps} steps (pc {t.pc:#x})")

    def step(self, t):
        pc = t.pc
        if pc in self.bp:
            self.bp[pc](self, t)
        x, text, pred = self.inst(pc)
        self.steps += 1
        if self.trace is not None:
            self.trace.append((pc, text))
            if len(self.trace) > self.trace_max:
                del self.trace[:len(self.trace) - self.trace_max]
        t.pc = pc + 1
        if pred == "pn":
            raise EmuError(f"{pc:#06x}: pn predicate")
        if pred is not None:
            neg, n = pred
            if t.p[n] == neg:
                return
        name = x.form.name
        d = x.d
        ops = x.ops
        f = getattr(self, "op_" + name.replace(".", "_"), None)
        if f is None:
            if self.on_unknown and self.on_unknown(self, t, x, text):
                return
            raise EmuError(f"{pc:#06x}: no semantics for `{text}`")
        f(t, x, d, ops, pc, text)
        if d.get("end"):
            raise Ended()

    # moves ----------------------------------------------------------
    def reps(self, d):
        return d.get("rpt", 1) if d.get("rcnt", 1) else 1

    def op_mov(self, t, x, d, ops, pc, text):
        n = d.get("rpt", 1) if d.get("rcnt") else 1
        for k in range(n):
            self.wr(t, ops[0], self.rd(t, ops[1], k), k)

    def op_limm(self, t, x, d, ops, pc, text):
        self.wr(t, ops[0], ops[1]["n"])

    # bitwise --------------------------------------------------------
    def src2(self, t, d, o):
        v = self.rd(t, o)
        if o["kind"] == "#":
            v = rol32(v, d.get("rot", 0))
        if d.get("inv"):
            v = ~v & M32
        return v

    def bitop(self, t, d, ops, f):
        n = d.get("rpt", 1) if d.get("rcnt") else 1
        for k in range(n):
            a = self.rd(t, ops[1], k)
            b = self.src2(t, d, ops[2]) if ops[2]["kind"] == "#" else self.src2(t, d, ops[2])
            self.wr(t, ops[0], f(a, b), k)

    def op_and(self, t, x, d, ops, pc, text):
        self.bitop(t, d, ops, lambda a, b: a & b)

    def op_or(self, t, x, d, ops, pc, text):
        self.bitop(t, d, ops, lambda a, b: a | b)

    def op_xor(self, t, x, d, ops, pc, text):
        self.bitop(t, d, ops, lambda a, b: a ^ b)

    def op_shl(self, t, x, d, ops, pc, text):
        self.bitop(t, d, ops, lambda a, b: (a << b) & M32 if b < 32 else 0)

    def op_shr(self, t, x, d, ops, pc, text):
        self.bitop(t, d, ops, lambda a, b: a >> b if b < 32 else 0)

    def op_asr(self, t, x, d, ops, pc, text):
        self.bitop(t, d, ops, lambda a, b: ((a - (1 << 32) if a >> 31 else a) >> min(b, 31)) & M32)

    def op_rol(self, t, x, d, ops, pc, text):
        self.bitop(t, d, ops, lambda a, b: rol32(a, b))

    # test -----------------------------------------------------------
    ALUF = {
        "and": (lambda a, b: a & b, 32), "or": (lambda a, b: a | b, 32),
        "xor": (lambda a, b: a ^ b, 32),
        "shl": (lambda a, b: (a << b) & M32 if b < 32 else 0, 32),
        "shr": (lambda a, b: a >> b if b < 32 else 0, 32),
        "rol": (lambda a, b: rol32(a, b), 32),
        "asr": (lambda a, b: ((a - (1 << 32) if a >> 31 else a) >> min(b, 31)) & M32, 32),
        "iadd16": (lambda a, b: (a + b) & 0xFFFF, 16), "isub16": (lambda a, b: (a - b) & 0xFFFF, 16),
        "iaddu16": (lambda a, b: (a + b) & 0xFFFF, 16), "isubu16": (lambda a, b: (a - b) & 0xFFFF, 16),
        "iadd32": (lambda a, b: (a + b) & M32, 32), "iaddu32": (lambda a, b: (a + b) & M32, 32),
    }

    def op_test(self, t, x, d, ops, pc, text):
        from hw_pack import ALU
        an = ALU[d["alu"]]
        if an not in self.ALUF:
            raise EmuError(f"{pc:#06x}: test alu {an}")
        if d.get("chan", 0) or d.get("mask", 1) != 1 or d.get("fmt"):
            raise EmuError(f"{pc:#06x}: test form {text}")
        # with a destination (no `nodst` field) the operands are p, dst, src1, src2
        dst = None if "nodst" in d else ops[1]
        s1, s2 = ops[-2], ops[-1]
        f, bits = self.ALUF[an]
        a = self.rd(t, s1)
        b = self.rd(t, s2)
        if bits == 16:
            a &= 0xFFFF
            b &= 0xFFFF
        r = f(a, b)
        c = d["cond"]
        sgn, zero, comb = c & 3, c >> 2 & 3, c >> 4 & 1
        neg = bool(r >> (bits - 1) & 1)
        if an == "isubu16":     # unsigned: the sign is the borrow
            neg = a < b
        conds = []
        if sgn:
            conds.append(neg if sgn == 1 else not neg)
        if zero:
            conds.append(r == 0 if zero == 1 else r != 0)
        if not conds:
            res = True
        elif comb:
            res = all(conds)
        else:
            res = any(conds)
        t.p[ops[0]["n"]] = res
        if dst is not None:
            self.wr(t, dst, r)

    # integer multiply-add -------------------------------------------
    def op_imae(self, t, x, d, ops, pc, text):
        n = d.get("rpt", 1)
        for k in range(n):
            a = self.rd(t, ops[1], k)
            b = self.rd(t, ops[2], k)
            c = self.rd(t, ops[3], k)
            a = a >> 16 if d.get("s0hi") else a & 0xFFFF
            b = b >> 16 if d.get("s1hi") else b & 0xFFFF
            if d.get("sgn"):
                a, b = s16(a), s16(b)
                if ops[2]["kind"] == "#":
                    b = ops[2]["n"]
            tt = d.get("t2", 2)
            if tt == 0:
                c = (c >> 16 if d.get("s2hi") else c) & 0xFFFF
            elif tt == 1:
                c = s16(c >> 16 if d.get("s2hi") else c)
            if d.get("cout") or d.get("cin"):
                raise EmuError(f"{pc:#06x}: imae carry")
            self.wr(t, ops[0], a * b + c, k)

    def op_ima32(self, t, x, d, ops, pc, text):
        a, b, c = (self.rd(t, o) for o in ops[1:4])
        if d.get("hi") or d.get("cin") or d.get("negp") or d.get("neg2"):
            raise EmuError(f"{pc:#06x}: ima32 form {text}")
        self.wr(t, ops[0], a * b + c)

    def op_ima16(self, t, x, d, ops, pc, text):
        a, b, c = (self.rd(t, o) for o in ops[1:4])
        if ops[3].get("neg"):
            c = -c
        if d.get("sgn"):
            a, b = s16(a), s16(b)
        else:
            a, b = a & 0xFFFF, b & 0xFFFF
        self.wr(t, ops[0], (a * b + c) & 0xFFFF)

    # memory ---------------------------------------------------------
    SIZE = {0: 4, 1: 2, 2: 1}

    def addrs(self, t, d, base_op, off_op):
        """the byte address of each repeat; pre/post increment update the base
        register. Rules (docs/UKERNEL.md, "Loads and stores"), from the
        microkernel's own use and a DDK capture of its writes:
          moe, no increment:        base + off
          moe, post-increment:      base; then base += off
          repeat n, post-increment: base + off + k; then base += n
          repeat n, no increment:   base + off + 1 + k"""
        sz = self.SIZE[d["size"]]
        n = d.get("rpt", 1)
        base = self.rd(t, base_op)
        off = self.rd(t, off_op)
        if d.get("preinc"):
            raise EmuError("ld/st pre-increment: no rule yet")
        if d.get("moe"):
            if n != 1:
                raise EmuError("ld/st moe with a repeat: no rule yet")
            if d.get("postinc"):
                self.wr(t, base_op, base + off * sz)
                return [base]
            return [(base + off * sz) & M32]
        if d.get("postinc"):
            self.wr(t, base_op, base + n * sz)
            return [(base + (off + k) * sz) & M32 for k in range(n)]
        return [(base + (off + 1 + k) * sz) & M32 for k in range(n)]

    def op_ld(self, t, x, d, ops, pc, text):
        if d["am"] != 0:
            raise EmuError(f"{pc:#06x}: ld mode {text}")
        sz = self.SIZE[d["size"]]
        for k, a in enumerate(self.addrs(t, d, ops[1], ops[2])):
            w = self.mem.rd(a & ~3, pc)
            if sz == 2:
                w = w >> (8 * (a & 2)) & 0xFFFF
            elif sz == 1:
                w = w >> (8 * (a & 3)) & 0xFF
            self.wr(t, ops[0], w, k)

    def op_st(self, t, x, d, ops, pc, text):
        if d["am"] != 0:
            raise EmuError(f"{pc:#06x}: st mode {text}")
        sz = self.SIZE[d["size"]]
        for k, a in enumerate(self.addrs(t, d, ops[0], ops[1])):
            v = self.rd(t, ops[2], k if ops[2]["kind"] != "#" else 0)
            if sz == 4:
                self.mem.wr(a, v, pc)
            else:
                w = self.mem.rd(a & ~3)
                sh = 8 * (a & 3)
                m = (0xFFFF if sz == 2 else 0xFF) << sh
                self.mem.wr(a & ~3, (w & ~m) | (v << sh & m), pc)

    def op_ldr(self, t, x, d, ops, pc, text):
        a = self.rd(t, ops[1]) * 4
        self.wr(t, ops[0], self.regs.rd(a, pc))

    def op_str(self, t, x, d, ops, pc, text):
        self.regs.wr(self.rd(t, ops[0]) * 4, self.rd(t, ops[1]), pc)

    # flow -----------------------------------------------------------
    def op_ba(self, t, x, d, ops, pc, text):
        if d.get("link"):
            t.link = pc + 1
        t.pc = ops[0]["n"]

    def op_br(self, t, x, d, ops, pc, text):
        if d.get("link"):
            t.link = pc + 1
        t.pc = pc + ops[0]["n"]

    def op_lapc(self, t, x, d, ops, pc, text):
        if t.link is None:
            raise EmuError(f"{pc:#06x}: lapc with no link")
        t.pc = t.link

    def op_savl(self, t, x, d, ops, pc, text):
        self.wr(t, ops[0], t.link if t.link is not None else 0xBADC0DE)

    def op_setl(self, t, x, d, ops, pc, text):
        t.link = self.rd(t, ops[0])

    def op_nop(self, t, x, d, ops, pc, text):
        pass

    op_idf = op_wdf = op_smlsi = op_nop

    def op_phas_i(self, t, x, d, ops, pc, text):
        if ops[0]["n"] != 0:
            raise EmuError(f"{pc:#06x}: phas.i to {ops[0]['n']}")

    op_phas = op_phas_i

    def op_emit(self, t, x, d, ops, pc, text):
        vals = [self.rd(t, o) for o in ops]
        self.emits.append((pc, d["tgt"], vals, dict(d)))

    def op_lock(self, t, x, d, ops, pc, text):
        self.lock_halt = True
        raise EmuError(f"{pc:#06x}: lock (the microkernel's halt)")
