"""Helpers for hwt.py suites: load values with limm, read a register out through o0."""
import struct

END = "[skipinv end mask=1]"


def F(x):
    """f32 bits"""
    return struct.unpack("<I", struct.pack("<f", x))[0]


def H(lo, hi=0.0):
    """two f16 in one word, lo in bits 0-15"""
    return struct.unpack("<I", struct.pack("<2e", lo, hi))[0]


def load(regs):
    """{'r0': word, ...} -> limm lines"""
    return [f"limm {r}, #{w:#x}  [skipinv]" for r, w in regs.items()]


def out(reg):
    return f"mov o0, {reg}  {END}"


def vec(base, vals):
    return {f"r{base + i}": F(v) for i, v in enumerate(vals)}
