"""A model of the USSE instructions the open compiler emits for integer and
conversion code, run on one instance, from the disassembler's text.

    python3 -I tools/usc/usse_sim.py PROGRAM.code pa0=1 pa4=0x10 ... [-o o4,o5]

The semantics are docs/ISA.md's: what is tagged hardware there is modelled as
measured, the rest as documented (oracle) or as the compiler assumes it
(flagged ASSUMED below). This checks that emitted sequences compute what
they should under those semantics; it is no substitute for hwt tests on the
hardware, which check the semantics themselves.

Covered: mov, limm, movc (z/nz/ltz/gez, i32/f32), fmad, fsubflr, fminmax,
fmaxmin, frcp, pck (f32/f16-free pairs: s16, u16, f32), test/testmask with
fsub/fadd and the bitwise ALU ops, and/or/xor/shl/shr/asr (immediates with
rot, inv), ima32 (hi, src2 negate), predicates, br to an offset.
Not covered: F16/C10 operands, repeats of anything but the float classes'
inc/moe, efo, fdual, smp, ld/st, phases.
"""
import math
import re
import struct
import subprocess
import sys

M32 = 0xffffffff
HWC = {4: 0x40000000, 5: 0x40800000, 6: 0x41000000, 7: 0x41800000, 8: 0x42000000,
       9: 0x42800000, 10: 0x43000000, 11: 0x43800000, 12: 0x44000000, 13: 0x44800000,
       14: 0x3f000000, 15: 0x3e800000, 16: 0x3e000000, 17: 0x3d800000, 18: 0x3d000000,
       19: 0x3c800000, 20: 0x3c000000, 21: 0x3b800000, 22: 0x3b000000, 23: 0x3a800000,
       24: 0x3a000000, 25: 0x39800000, 26: 0x39000000, 27: 0x38800000, 39: 0x37800000,
       45: 0x3c003c00, 48: 0, 49: 0, 50: 0, 51: 0, 52: 0x3f800000, 53: 0x3f800000,
       54: 0x3f800000, 55: 0x3f800000, 56: M32}


def f(b):
    """f32 bits to a float, denormals as zero (ISA.md: movc z is true for one)"""
    b &= M32
    if (b >> 23) & 0xff == 0:
        return -0.0 if b >> 31 else 0.0
    return struct.unpack("<f", struct.pack("<I", b))[0]


def b(x):
    """a float to f32 bits, rounded to nearest; denormals flush to zero"""
    try:
        r = struct.unpack("<I", struct.pack("<f", x))[0]
    except OverflowError:
        r = 0xff800000 if x < 0 else 0x7f800000
    if (r >> 23) & 0xff == 0:
        r &= 0x80000000
    return r


def s32(v):
    v &= M32
    return v - (1 << 32) if v >> 31 else v


class Sim:
    def __init__(self, regs):
        self.r = dict(regs)
        self.p = [False] * 4

    def rd(self, o, step=0):
        neg = absf = False
        o = o.strip()
        if o.startswith("-"):
            neg, o = True, o[1:]
        if o.startswith("|"):
            absf, o = True, o.strip("|")
        lane = 0
        m = re.match(r"(.*)\.l(\d)$", o)
        if m:
            o, lane = m.group(1), int(m.group(2))
        if o.startswith("#"):
            v = int(o[1:], 0) & M32
            return v, neg, absf, lane, True
        m = re.match(r"([a-z]+)(\d+)$", o)
        bank, n = m.group(1), int(m.group(2)) + step
        if bank == "c":
            v = HWC[n]
        else:
            v = self.r.get((bank, n))
            if v is None:
                raise KeyError(f"read of {bank}{n} before a write")
        return v, neg, absf, lane, False

    def fv(self, o, step=0):
        v, neg, absf, _, _ = self.rd(o, step)
        x = f(v)
        if absf:
            x = abs(x)
        return -x if neg else x

    def wr(self, o, v, step=0):
        m = re.match(r"([a-z]+)(\d+)$", o.strip())
        self.r[(m.group(1), int(m.group(2)) + step)] = v & M32

    def run(self, lines):
        pc = 0
        n = 0
        while pc < len(lines):
            n += 1
            if n > 100000:
                raise RuntimeError("runaway")
            pc = self.step(lines[pc], pc)

    def step(self, line, pc):
        m = re.match(r"(?:\((!?)p(\d)\)\s+)?(\S+)\s*(.*?)\s*(?:\[(.*)\])?\s*$", line)
        inv_p, pn, op, args, flags = m.groups()
        flags = flags or ""
        fl = dict((k, v) for k, _, v in (x.partition("=") for x in flags.split()))
        if pn is not None and self.p[int(pn)] == bool(inv_p):
            return pc + 1
        a = [x for x in re.split(r",\s*", args) if x] if args else []
        base = op.split(".")[0]
        if base == "br":
            return pc + int(a[0], 0)
        rpt = int(fl.get("rpt", 1))
        for k in range(rpt):
            st = [0, 0, 0, 0]
            if "moe" in fl or "rcnt" in fl:
                st = [k] * 4
            else:
                st[0] = k
                for j in range(3):
                    if f"inc{j}" in fl:
                        st[1 + j] = k
            self.one(op, base, a, fl, st)
        return pc + 1

    def one(self, op, base, a, fl, st):
        parts = op.split(".")
        if base == "mov":
            self.wr(a[0], self.rd(a[1], st[1])[0], st[0])
        elif base == "limm":
            self.wr(a[0], int(a[1][1:], 0))
        elif base == "movc":
            cond, ty = parts[1], parts[2]
            v = self.rd(a[1], st[1])[0]
            if ty == "i32":
                t = {"z": v == 0, "nz": v != 0, "ltz": s32(v) < 0, "gez": s32(v) >= 0}[cond]
            elif ty == "f32":
                x = f(v)
                nan = (v >> 23) & 0xff == 0xff and v & 0x7fffff
                t = {"z": x == 0 and not nan, "nz": x != 0 or nan, "ltz": x < 0 and not nan,
                     "gez": x >= 0 and not nan}[cond]
            else:
                raise NotImplementedError(op)
            self.wr(a[0], self.rd(a[2] if t else a[3], st[2 if t else 3])[0], st[0])
        elif base in ("fmad", "fsubflr", "fminmax", "fmaxmin", "frcp"):
            if base == "frcp":
                x = self.fv(a[1], st[1])
                r = math.inf if x == 0 else 1.0 / x
            else:
                x, y = self.fv(a[1], st[1]), self.fv(a[2], st[2])
                if base == "fsubflr":
                    r = x - math.floor(y)
                else:
                    z = self.fv(a[3], st[3])
                    if base == "fmad":
                        r = f(b(x * y)) + z
                    elif base == "fminmax":
                        r = max(min(x, y), z)
                    else:
                        r = min(max(x, y), z)
            self.wr(a[0], b(r), st[0])
        elif base == "pck":
            self.pck(parts[1], parts[2], a, fl)
        elif base in ("and", "or", "xor", "shl", "shr", "asr"):
            x = self.rd(a[1])[0]
            y, _, _, _, imm = self.rd(a[2])
            if imm:
                r = int(fl.get("rot", 0))
                y = ((y << r) | (y >> (32 - r))) & M32 if r else y
            if "inv" in parts:
                y = ~y & M32
            if base == "and":
                v = x & y
            elif base == "or":
                v = x | y
            elif base == "xor":
                v = x ^ y
            elif base == "shl":
                v = (x << y) & M32 if y < 32 else 0  # hardware: 33 gives 0
            elif base == "shr":
                v = x >> y if y < 32 else 0
            else:
                v = (s32(x) >> min(y, 31)) & M32
            self.wr(a[0], v)
        elif base == "ima32":
            x = self.rd(a[1])[0]
            y = self.rd(a[2])
            e = self.rd(a[3])
            sgn = "sgn" in parts
            # ASSUMED: immediates are 7-bit, signed with sgn (ISA.md, oracle)
            yv = y[0]
            if y[4] and sgn and yv & 0x40:
                yv -= 0x80
            ev = e[0]
            if e[4] and sgn and ev & 0x40:
                ev -= 0x80
            xv = s32(x) if sgn else x
            yv = s32(yv) if sgn and not y[4] else yv
            ev = -ev if e[1] else ev
            full = xv * yv + (s32(ev) if sgn else ev)
            self.wr(a[0], full)
            hi = fl.get("hi")
            if hi:
                self.r[("i", int(hi[1]))] = (full >> 32) & M32
        elif base in ("test", "testmask"):
            alu, cond = parts[1], parts[2]
            if base == "test":
                pd, x, y = a[0], a[1], a[2]
            else:
                pd, x, y = a[0], a[1], a[2]
            r = self.alu(alu, x, y)
            t = self.cond(cond, r)
            if base == "test":
                self.p[int(pd[1])] = t
            else:
                w = {"byte": 0xff, "word": 0xffff, "dword": M32}[parts[3]]
                self.wr(pd, w if t else 0)
        elif base == "nop":
            pass
        else:
            raise NotImplementedError(op)

    def alu(self, alu, x, y):
        """(sign, zero) of the test's ALU result"""
        if alu in ("fsub", "fadd"):
            r = self.fv(x) - self.fv(y) if alu == "fsub" else self.fv(x) + self.fv(y)
            return (r < 0 or (r == 0 and math.copysign(1, r) < 0 and False), r == 0)
        xv = self.rd(x)[0]
        yv = self.rd(y)[0]
        v = {"and": xv & yv, "or": xv | yv, "xor": xv ^ yv}[alu]
        return (v >> 31 == 1, v == 0)

    @staticmethod
    def cond(c, r):
        sgn, zero = r
        return {"lt": sgn and not zero, "le": sgn or zero, "eq": zero, "ne": not zero,
                "gt": not sgn and not zero, "ge": not sgn or zero, "neg": sgn,
                "pos": not sgn}[c]

    def pck(self, dst, src, a, fl):
        vals = []
        for o in a[1:3]:
            v, neg, absf, lane, imm = self.rd(o)
            if src == "f32":
                vals.append(f(v))
            elif src in ("u16", "s16"):
                h = (v >> (16 * (lane // 2))) & 0xffff
                if src == "s16" and h >> 15:
                    h -= 0x10000
                vals.append(float(h))
            else:
                raise NotImplementedError(src)
        if dst == "f32":
            x = vals[0]
            if src == "f32" and "rint" in fl:
                rnd = fl.get("rnd", "nearest")
                x = {"zero": math.trunc, "down": math.floor, "up": math.ceil,
                     "nearest": lambda v: round(v)}[rnd](x) if math.isfinite(x) else x
            self.wr(a[0], b(float(x)))
        elif dst in ("u16", "s16"):
            out = 0
            for i, x in enumerate(vals):
                x = round(x)  # nearest-even (hardware, hwt_p4, s16)
                if dst == "u16":
                    x = min(max(x, 0), 0xffff)
                else:
                    x = min(max(x, -0x8000), 0x7fff) & 0xffff
                out |= x << (16 * i)
            self.wr(a[0], out)
        else:
            raise NotImplementedError(dst)


def disasm(code, disasm_tool):
    words = b""
    for line in open(code):
        hi, lo = line.split()
        words += struct.pack("<II", int(lo, 16), int(hi, 16))
    out = subprocess.run([disasm_tool, "/dev/stdin"], input=words, capture_output=True, check=True)
    return [l for l in out.stdout.decode().splitlines() if l.strip()]


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("code")
    ap.add_argument("regs", nargs="*", help="reg=value (pa0=1, sa9=0x10, pa1=f:2.5)")
    ap.add_argument("--disasm", default="sgx-disasm")
    ap.add_argument("-o", default="o4,o5,o6,o7")
    args = ap.parse_args()
    regs = {}
    for r in args.regs:
        k, v = r.split("=")
        m = re.match(r"([a-z]+)(\d+)$", k)
        regs[(m.group(1), int(m.group(2)))] = b(float(v[2:])) if v.startswith("f:") else int(v, 0) & M32
    s = Sim(regs)
    s.run(disasm(args.code, args.disasm))
    for o in args.o.split(","):
        print(o, hex(s.rd(o)[0]))


if __name__ == "__main__":
    main()
