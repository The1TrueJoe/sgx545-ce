"""Read a pvrtrace log (re/trace/pvrtrace.c) into records."""
import os
import struct
from dataclasses import dataclass, field

REC_BRIDGE, REC_MMAP, REC_MUNMAP, REC_SNAP, REC_NOTE, REC_UBUF = 1, 2, 3, 4, 5, 6
SNAP_REASONS = {1: "kick", 2: "unmap", 3: "exit", 4: "init", 5: "xfer"}

# Bridge index -> name for this build, generated from the headers by re/abi/ids.sh.
BRIDGE_NAMES = {}
for _ln in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "abi", "bridge-ids.txt")):
    _i, _n = _ln.split()
    BRIDGE_NAMES.setdefault(int(_i), _n)


@dataclass
class Bridge:
    seq: int
    id: int
    ret: int
    err: int
    t_ns: int
    inp: bytes
    out: bytes
    services: int = 0

    @property
    def nr(self):
        return self.id & 0xFF

    @property
    def name(self):
        return BRIDGE_NAMES.get(self.nr, f"bridge{self.nr}")


@dataclass
class Mmap:
    seq: int
    addr: int
    length: int
    prot: int
    flags: int
    off: int


@dataclass
class Munmap:
    seq: int
    addr: int
    length: int


@dataclass
class Snap:
    seq: int
    reason: str
    kick: int
    addr: int
    data: bytes


@dataclass
class Note:
    seq: int
    text: str


@dataclass
class UBuf:
    seq: int
    nr: int
    phase: int   # 0: read by the kernel (before the call), 1: written by it (after)
    ptr: int
    data: bytes


@dataclass
class Log:
    ptr: int
    recs: list = field(default_factory=list)

    def of(self, cls):
        return [r for r in self.recs if isinstance(r, cls)]


def load(path):
    b = open(path, "rb").read()
    assert b[:4] == b"PVRT", "not a pvrtrace log"
    ver, ptr = struct.unpack_from("<II", b, 4)
    hdr = 36 if ver >= 2 else 28
    log = Log(ptr)
    o = 12
    seq = 0
    while o + 8 <= len(b):
        typ, n = struct.unpack_from("<II", b, o)
        o += 8
        p = b[o:o + n]
        o += n
        if len(p) < n:
            break  # truncated tail: the process died mid-record
        if typ == REC_BRIDGE:
            bid, ret, err, ni, no = struct.unpack_from("<IiiII", p, 0)
            (t,) = struct.unpack_from("<Q", p, 20)
            svc = struct.unpack_from("<Q", p, 28)[0] if ver >= 2 else 0
            log.recs.append(Bridge(seq, bid, ret, err, t, p[hdr:hdr + ni], p[hdr + ni:hdr + ni + no], svc))
        elif typ == REC_MMAP:
            a, ln, pf, off, _r = struct.unpack_from("<QQQQQ", p, 0)
            log.recs.append(Mmap(seq, a, ln, pf & 0xFFFFFFFF, pf >> 32, off))
        elif typ == REC_MUNMAP:
            a, ln = struct.unpack_from("<QQ", p, 0)
            log.recs.append(Munmap(seq, a, ln))
        elif typ == REC_SNAP:
            why, k, a, ln = struct.unpack_from("<IIQQ", p, 0)
            log.recs.append(Snap(seq, SNAP_REASONS.get(why, str(why)), k, a, p[24:24 + ln]))
        elif typ == REC_UBUF:
            nr, ph, a = struct.unpack_from("<IIQ", p, 0)
            log.recs.append(UBuf(seq, nr, ph, a, p[16:]))
        elif typ == REC_NOTE:
            log.recs.append(Note(seq, p.decode(errors="replace")))
        seq += 1
    return log
