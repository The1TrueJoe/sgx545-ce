"""Struct layouts from clang's -fdump-record-layouts (re/abi/layouts.sh).

layout("x86")["_SGX_CCB_KICK_"] -> Record with .size and .fields, where each
field is (offset, depth, type, name) exactly as the compiler laid it out for
that ABI. get(rec, buf, "sCommand.ui32Data") reads a dotted path.
"""
import os
import re
import struct
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
ABI = os.path.join(HERE, "..", "abi")

_line = re.compile(r"^\s*(\d+)(?::(\d+)-(\d+))? \| (\s*)(.*)$")
_size = re.compile(r"\[sizeof=(\d+)")


@dataclass
class Field:
    off: int
    depth: int
    type: str
    name: str


@dataclass
class Record:
    name: str
    size: int
    fields: list

    def path(self, dotted):
        """Offset and type of a dotted field path, e.g. 'sCommand.ui32Data[2]'."""
        parts = dotted.split(".")
        depth, start = 1, 0
        off = None
        for i, p in enumerate(parts):
            idx = 0
            m = re.match(r"(\w+)\[(\d+)\]$", p)
            if m:
                p, idx = m.group(1), int(m.group(2))
            for j in range(start, len(self.fields)):
                f = self.fields[j]
                if f.depth < depth:
                    raise KeyError(dotted)
                if f.depth == depth and f.name == p:
                    off = f.off
                    if idx:
                        off += idx * _elem_size(self, j)
                    start, depth = j + 1, depth + 1
                    ftype = f.type
                    break
            else:
                raise KeyError(dotted)
        return off, ftype


def _elem_size(rec, j):
    f = rec.fields[j]
    m = re.search(r"\[(\d+)\]$", f.type)
    n = int(m.group(1)) if m else 1
    nxt = next((g.off for g in rec.fields[j + 1:] if g.depth <= f.depth), rec.size)
    return (nxt - f.off) // n


_cache = {}


def layout(arch):
    if arch in _cache:
        return _cache[arch]
    recs = {}
    cur = None
    for ln in open(os.path.join(ABI, f"layout-{arch}.txt")):
        if ln.startswith("*** Dumping"):
            cur = None
            continue
        m = _size.search(ln)
        if m and cur is not None:
            cur.size = int(m.group(1))
            recs.setdefault(cur.name, cur)
            continue
        m = _line.match(ln)
        if not m:
            continue
        off, depth_ws, text = int(m.group(1)), m.group(4), m.group(5)
        depth = len(depth_ws) // 2
        if cur is None:
            name = text.split()[-1]
            cur = Record(name, 0, [])
            continue
        # "struct _X_ sName", "IMG_UINT32 ui32Foo", "IMG_UINT32[4] aui32" , "union (anonymous) u"
        bits = text.rsplit(" ", 1)
        typ, nm = (bits[0], bits[1]) if len(bits) == 2 else (text, "")
        cur.fields.append(Field(off, depth, typ, nm))
    _cache[arch] = recs
    return recs


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def ptr(arch, buf, off):
    return struct.unpack_from("<Q" if arch == "x86_64" else "<I", buf, off)[0]


def get(arch, recname, buf, dotted, base=0):
    rec = layout(arch)[recname]
    off, typ = rec.path(dotted)
    if "*" in typ or typ.endswith("HANDLE") or typ in ("IMG_PVOID", "IMG_SIZE_T", "IMG_UINTPTR_T"):
        return ptr(arch, buf, base + off)
    if "UINT64" in typ:
        return struct.unpack_from("<Q", buf, base + off)[0]
    return u32(buf, base + off)


def dump(arch, recname, buf, base=0, leaf_only=True):
    """Yield (path, offset, type, value) for every scalar leaf of a record."""
    rec = layout(arch)[recname]
    stack = []
    for i, f in enumerate(rec.fields):
        del stack[f.depth - 1:]
        stack.append(f.name)
        nxt = rec.fields[i + 1] if i + 1 < len(rec.fields) else None
        is_parent = nxt is not None and nxt.depth > f.depth
        if is_parent and leaf_only:
            continue
        o = base + f.off
        if o + 4 > len(buf):
            continue
        if "*" in f.type or "HANDLE" in f.type or f.type in ("IMG_PVOID", "IMG_SIZE_T"):
            v = ptr(arch, buf, o)
        else:
            v = u32(buf, o)
        yield ".".join(stack), f.off, f.type, v
