"""A PDS program interpreter for the microkernel emulator.

The EDM runs a PDS program (docs/PDS.md) to start a USSE task: it fills the
task's attributes (DOUTD DMAs device memory into the secondary attributes,
DOUTA writes a primary-attribute register) and DOUTU launches the task. This
runs that program against the emulator's device memory and then starts the
USSE task in emu.py with those attributes, so the whole event-program ->
microkernel path can be checked offline.

It models the data-master effects (DOUTD/DOUTA/DOUTU), the small ALU/TST/BRA
the entry programs use, and HALT. What it does NOT model is whether a given
invocation context (an EVENT_KICK vs an emit from the microkernel) actually
executes a DOUTD/DOUTA — that is hardware behaviour the disassembly doesn't
show. So a program that runs cleanly here is logically correct; whether the
hardware runs it in the event-kick context is a separate question.

    run(code_bytes, data_words, emu, mem, inputs={0:...}, start=task_runner)
"""
import struct

HALT = 0xAF000000


def ds_layout(words, data_words):
    """ds[bank][word] from the program's data segment (16-byte rows DS0 DS0
    DS1 DS1), plus the inputs that follow DS0 word 0x3f."""
    ds = {0: {}, 1: {}}
    for k in range(data_words):
        row, col = k // 2, k % 2
        if row * 4 + 3 < len(words):
            ds[0][2 * row + col] = words[row * 4 + col]
            ds[1][2 * row + col] = words[row * 4 + 2 + col]
    return ds


def movs_sources(w):
    """the four source words a MOVS sends, as (bank, word) selectors"""
    sw = [(w >> 11) & 3, (w >> 9) & 3, (w >> 7) & 3, (w >> 5) & 3]
    r2, r1 = (w >> 13) & 0x1F, (w >> 18) & 0x3F
    out = []
    for sel in sw:
        if sel < 2:
            out.append((0, 2 * r1 + sel))
        else:
            out.append((1, 2 * r2 + sel - 2))
    return out


class Doutu:
    def __init__(self, word0, word1, word2):
        self.w0, self.w1, self.w2 = word0, word1, word2


class PDS:
    DOUTD, DOUTU, DOUTA = 3, 5, 6

    def __init__(self, code, data_words, mem, inputs=None, code_base=0x30000000):
        self.words = list(struct.unpack(f"<{len(code) // 4}I", code))
        self.data_words = data_words
        self.ds = ds_layout(self.words, data_words)
        self.inp = dict(inputs or {})
        self.mem = mem
        self.code_base = code_base
        self.code = self.words[data_words:]
        # effects accumulated until DOUTU launches the task
        self.sa = {}            # secondary attribute index -> value
        self.pa = {}            # primary attribute index -> value
        self.doutu = None
        self.log = []

    def read(self, bank, word):
        if bank == 0 and word >= 0x40:
            return self.inp.get(word - 0x40, 0)
        return self.ds[bank].get(word, 0)

    def run(self, max_steps=4096):
        pc = 0
        for _ in range(max_steps):
            if pc >= len(self.code):
                raise RuntimeError("PDS ran off the end (no HALT)")
            w = self.code[pc]
            pc += 1
            if w == HALT:
                return self
            op, s, cc = w >> 28, (w >> 27) & 1, (w >> 24) & 7
            if cc == 0 and not self.p0:          # (p0) predicate, false -> skip
                if op in (0, 2, 4, 6, 8, 0xC, 0xD, 0xF):
                    continue
            if op in (0, 2):
                self.movs(w)
            elif op in (4, 6, 0xC, 0xD):
                self.alu(op, s, w)
            elif op == 8:
                self.tst(w)
            elif op == 9:
                t = w & 0xFFFFFF
                if cc == 7 or (cc == 0 and self.p0):
                    pc = t
            elif op == 0xF:
                self.shift(s, w)
            else:
                raise RuntimeError(f"PDS: unknown word {w:#010x}")
        raise RuntimeError("PDS step limit")

    p0 = False

    def val(self, bank, word):
        return self.read(bank, word)

    def movs(self, w):
        dest = w & 0x1F
        outs = [self.val(b, n) for (b, n) in movs_sources(w)]
        if dest == self.DOUTD:
            src, ctrl = outs[0], outs[1]
            n = (ctrl & 0xFF) + 1
            reg = (ctrl >> 8) & 0x3FFF
            to_sa = bool(ctrl & (1 << 25))
            off = outs[2] if (w >> 28) == 2 else 0     # the x2 third word is a byte offset
            for i in range(n):
                v = self.mem.rd((src + off + 4 * i) & 0xFFFFFFFF)
                (self.sa if to_sa else self.pa)[reg + i] = v
            self.log.append(("doutd", src + off, n, "sa" if to_sa else "pa", reg))
        elif dest == self.DOUTA:
            value, ctrl = outs[0], outs[1]
            self.pa[(ctrl >> 8) & 0xFFF] = value
            self.log.append(("douta", (ctrl >> 8) & 0xFFF, value))
        elif dest == self.DOUTU:
            self.doutu = Doutu(outs[0], outs[1], outs[2])
            self.log.append(("doutu", outs[0], outs[1]))
        else:
            self.log.append(("dout%d" % dest, outs))

    def alu(self, op, s, w):
        dst, s2, s1 = w & 0x7F, (w >> 9) & 0x7F, (w >> 16) & 0xFF
        a = self.read16(0, s1)
        b = self.read16(1, s2)
        if op == 4:
            r = (a - b) if s else (a + b)
        elif op == 6:
            r = (a & 0xFFFF) * (b & 0xFFFF)
        elif op == 0xC:
            r = (a & b) if s else (a | b)
        else:
            r = a ^ b
        self.write_u(dst, r & 0xFFFFFFFF)

    def read16(self, bank, f):
        return self.read(bank, f >> 1)      # 16-bit unit; we keep full 32-bit words

    def write_u(self, f, v):
        if f < 0x40:
            self.ds[0][f] = v
        elif f < 0x80:
            self.ds[1][f - 0x40] = v
        else:
            self.inp[f - 0x80] = v

    def tst(self, w):
        s1, s2, sel = (w >> 16) & 0xFF, (w >> 9) & 0x7F, (w >> 8) & 1
        v = self.read(1, s2 >> 1) if sel else self.read(0, s1 >> 1)
        self.p0 = (v == 0)

    def shift(self, s, w):
        src, amt, dst = (w >> 16) & 0xFF, (w >> 8) & 0xFF, w & 0xFF
        v = self.read(src >> 6, src & 0x3F) if src < 0x80 else self.inp.get(src - 0x80, 0)
        self.write_u(dst, (v >> amt if s else (v << amt) & 0xFFFFFFFF))


def use_addr(w0, pixel_base=0x37000000, vertex_base=0x3C000000, code_base=0x30000000):
    """DOUTU word0 -> code address. base 15 is the kernel code heap."""
    idx, off = w0 & 0xF, ((w0 >> 4) & 0x3FFFF) << 3
    if idx == 15:
        return code_base + off
    if idx >= 10:
        return vertex_base + (idx - 10) * 0x80000 + off
    return pixel_base + idx * 0x80000 + off


def launch(emu, prog, code_base=0x30000000):
    """apply a finished PDS program's attributes to the emulator and run the
    USSE task its DOUTU starts; returns the task"""
    if prog.doutu is None:
        raise RuntimeError("PDS program started no task (no DOUTU)")
    for i, v in prog.sa.items():
        emu.sa[i & 127] = v
    pc = use_addr(prog.doutu.w0, code_base=code_base)
    idx = (pc - emu.base) // 8
    pa = [prog.pa.get(i, 0) for i in range(max(prog.pa, default=-1) + 1)]
    return emu.task(idx, pa=pa)
