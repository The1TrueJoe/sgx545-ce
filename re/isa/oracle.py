"""Run libusc's own USE assembler (UseAssembleInstruction, libusc + 0x109f50)
in an i386 emulator, so any USEASM instruction can be encoded on the Mac.

  from oracle import Oracle
  o = Oracle("libusc.so.1.7.862890")
  ret, lo, hi, errs = o.encode(inst)      # inst: up to 64 dwords of USE_INST

The image is mapped as the dynamic loader would map it (RELATIVE relocations
applied); its few imports (memcpy, memset, ...) are stubbed in Python. The
target argument is libusc's own SGX545 entry from its core table (file vaddr
0x1498a0: core 9, features at 0x14de40). The context argument is a table of
callbacks: slot 6 is the encoder's error report (called with (ctx[0], 0, 0);
the messages were compiled out), the others are recorded if called. errs is
the number of error reports; libusc treats any as a failed encode.

Needs the 'unicorn' Python package.
"""
import os
import struct
import sys

# this directory's dis.py shadows the standard library's, which unicorn imports
_here = os.path.dirname(os.path.abspath(__file__))
_path = sys.path[:]
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != _here]
sys.modules.pop("dis", None)
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE  # noqa: E402
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX  # noqa: E402
sys.path = _path

BASE = 0x10000000
STUBS = 0x0F000000
STACK = 0x0E000000
HEAP = 0x0D000000
ENC = 0x109F50
TARGET = 0x1498A0
STOP = STUBS + 0xFF0


def elf32(d):
    """-> (PT_LOAD (vaddr, memsz, offset, filesz)), relocations (off, type, sym), dynsym names"""
    phoff, shoff = struct.unpack_from("<I", d, 0x1C)[0], struct.unpack_from("<I", d, 0x20)[0]
    phnum, shnum = struct.unpack_from("<H", d, 0x2C)[0], struct.unpack_from("<H", d, 0x30)[0]
    segs = []
    for i in range(phnum):
        t, o, v, _, fs, ms = struct.unpack_from("<6I", d, phoff + 32 * i)
        if t == 1:
            segs.append((v, ms, o, fs))
    sh = [struct.unpack_from("<10I", d, shoff + 40 * i) for i in range(shnum)]
    rels, names = [], {}
    for s in sh:
        if s[1] == 11:          # SHT_DYNSYM
            strtab = sh[s[6]]
            for k in range(s[5] // 16):
                nm = struct.unpack_from("<I", d, s[4] + 16 * k)[0]
                e = d.index(b"\0", strtab[4] + nm)
                names[k] = d[strtab[4] + nm:e].decode()
        if s[1] == 9:           # SHT_REL
            for k in range(s[5] // 8):
                off, info = struct.unpack_from("<2I", d, s[4] + 8 * k)
                rels.append((off, info & 0xFF, info >> 8))
    return segs, rels, names


class Oracle:
    def __init__(self, lib=None):
        lib = lib or os.environ.get("LIBUSC") or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                "libusc.so.1.7.862890")
        self.mu = mu = Uc(UC_ARCH_X86, UC_MODE_32)
        data = open(lib, "rb").read()
        segs, rels, syms = elf32(data)
        top = max(v + m for v, m, _, _ in segs)
        mu.mem_map(BASE, (top + 0xFFFF) & ~0xFFFF)
        for v, m, o, n in segs:
            mu.mem_write(BASE + v, data[o:o + n])
        mu.mem_map(STUBS, 0x1000)
        mu.mem_write(STUBS, b"\xc3" * 0x1000)          # every stub is a 'ret'
        mu.mem_map(STACK, 0x100000)
        mu.mem_map(HEAP, 0x100000)
        self.imports = {}
        for off, t, sym in rels:
            if t == 8:      # R_386_RELATIVE
                v = struct.unpack("<I", mu.mem_read(BASE + off, 4))[0]
                mu.mem_write(BASE + off, struct.pack("<I", v + BASE))
            elif t in (6, 7):   # GLOB_DAT, JUMP_SLOT
                name = syms[sym]
                i = len(self.imports)
                self.imports[STUBS + 0x10 * i] = name
                mu.mem_write(BASE + off, struct.pack("<I", STUBS + 0x10 * i))
        # context: [0] user word, [1..15] callbacks at STUBS+0x800+4*i
        self.ctx = HEAP + 0x1000
        mu.mem_write(self.ctx, struct.pack("<16I", 0x1234, *[STUBS + 0x800 + 0x10 * i for i in range(1, 16)]))
        self.calls = []
        mu.hook_add(UC_HOOK_CODE, self._stub, begin=STUBS, end=STUBS + 0xFFF)

    def _arg(self, n):
        esp = self.mu.reg_read(UC_X86_REG_ESP)
        return struct.unpack("<I", self.mu.mem_read(esp + 4 + 4 * n, 4))[0]

    def _stub(self, mu, addr, size, _):
        if addr == STOP:
            mu.emu_stop()
            return
        if addr >= STUBS + 0x800:
            self.calls.append((addr - STUBS - 0x800) // 0x10)
            mu.reg_write(UC_X86_REG_EAX, 0)
            return
        name = self.imports.get(addr)
        if name in ("memcpy", "memmove", "__memcpy_chk"):
            d, s, n = self._arg(0), self._arg(1), self._arg(2)
            mu.mem_write(d, bytes(mu.mem_read(s, n)))
            mu.reg_write(UC_X86_REG_EAX, d)
        elif name == "memset":
            d, c, n = self._arg(0), self._arg(1), self._arg(2)
            mu.mem_write(d, bytes([c & 0xFF]) * n)
            mu.reg_write(UC_X86_REG_EAX, d)
        else:
            raise RuntimeError(f"libusc called {name}")

    def encode(self, inst, offset=0, base=0):
        """inst: list of up to 64 dwords (USE_INST). -> (ret, lo, hi, n_errors)"""
        mu = self.mu
        w = list(inst) + [0] * (64 - len(inst))
        pinst, pout = HEAP + 0x2000, HEAP + 0x3000
        mu.mem_write(pinst, struct.pack("<64I", *[x & 0xFFFFFFFF for x in w]))
        mu.mem_write(pout, b"\0" * 64)
        sp = STACK + 0xF0000
        args = [BASE + TARGET, pinst, base, pout, offset, self.ctx]
        mu.mem_write(sp, struct.pack("<7I", STOP, *args))
        mu.reg_write(UC_X86_REG_ESP, sp)
        self.calls = []
        mu.emu_start(BASE + ENC, STOP, count=2_000_000)
        ret = mu.reg_read(UC_X86_REG_EAX)
        lo, hi = struct.unpack("<2I", mu.mem_read(pout, 8))
        return ret, lo, hi, self.calls.count(6), list(self.calls)


if __name__ == "__main__":
    import sys
    o = Oracle(sys.argv[1] if len(sys.argv) > 1 else None)
    # the and.test from the secondary-program prologue, as libusc was given it on the EA3
    inst = [int(x, 16) for x in "4f 40000201 0 0 18 0 6 10000 0 1 0 c 0 0 0 0 8 0 0 0 0 8 0 0 0".split()]
    print(o.encode(inst), "want lo a00c0000 hi 488b1281")
