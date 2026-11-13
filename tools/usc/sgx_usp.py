"""Read back the USP files sgx_usp.c writes (sgxc's output for the A/B harness).

    import sgx_usp
    s = sgx_usp.load(path)    # {"type", "n_temps", "pre", "blocks"}

The layout is sgx_usp.c's, field for field: a 0x84-byte header, list A (12
bytes per iterated input), list C (10-byte records for kinds 3 and 0x11, 8
otherwise), the secondary update program, the vertex input mask, the output
mask, then the blocks until type 7. Blocks: {"type": "code", "flags", "insts"},
{"type": "branch", "insts", "label"}, {"type": "label", "label"},
{"type": "sample5", "dependent", "smp"}, {"type": "sample6"}. An instruction
is (low word, high word). Anything sgx_usp.c doesn't write is refused.
"""
import struct

MAGIC = 0x55535020


class _R:
    def __init__(self, d):
        self.d, self.o = d, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.o)
        self.o += struct.calcsize("<" + fmt)
        return v

    def u16(self):
        return self.take("H")[0]

    def u32(self):
        return self.take("I")[0]

    def inst(self):
        return self.take("II")


def _moe(r):
    """format control (sgx_usp.c writes 2, C10 on), 4 MOE increments, 4 bases"""
    v = r.take("9H")
    return v[0]


def load(path):
    d = open(path, "rb").read()
    r = _R(d)
    if r.u32() != MAGIC:
        raise ValueError(f"{path}: not a USP")
    h = lambda off, f="H": struct.unpack_from("<" + f, d, off)[0]  # noqa: E731
    if h(0x28, "I") or h(0x52, "I") or h(0x56):
        raise ValueError(f"{path}: workaround ids, list B or list D: not written by sgx_usp.c")
    s = {"type": "vert" if h(0x10, "I") == 1 else "frag", "n_temps": h(0x46), "blocks": []}
    r.o = 0x84
    r.o += 12 * h(0x4e)                       # list A
    for _ in range(h(0x50)):                  # list C
        kind = r.u16()
        r.o += 8 if kind in (3, 0x11) else 6
    s["pre"] = [r.inst() for _ in range(h(0x7c))]
    if s["type"] == "vert":
        r.o += 4 * ((h(0x80) + 31) // 32)     # input mask
    r.o += 4 * ((h(0x82) + 31) // 32)         # output mask
    while True:
        t = r.u32()
        if t == 7:
            break
        if t == 2:
            n = r.u16()
            fc = _moe(r)
            r.o += 2 * n                      # per-instruction flags
            s["blocks"].append({"type": "code", "flags": fc, "insts": [r.inst() for _ in range(n)]})
        elif t == 3:
            w = r.inst()
            s["blocks"].append({"type": "branch", "insts": [w], "label": r.u16()})
        elif t == 4:
            s["blocks"].append({"type": "label", "label": r.u16()})
        elif t == 5:
            dependent = r.u16() == 0
            _moe(r)
            r.o += 4 + 2 * 12                 # id, iflags .. forced format
            b = {"type": "sample5", "dependent": dependent}
            if dependent:
                b["smp"] = r.inst()           # the smp template
            else:
                r.o += 8                      # set, dim, coordinate
            s["blocks"].append(b)
        elif t == 6:
            _moe(r)
            r.o += 4 + 2 * 26
            s["blocks"].append({"type": "sample6"})
        else:
            raise ValueError(f"{path}: block type {t} at {r.o - 4:#x}: not written by sgx_usp.c")
    return s
