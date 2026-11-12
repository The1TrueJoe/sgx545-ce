"""Assemble SGX545 USSE instructions: the inverse of dis.py.

  python3 asm.py '(p0) fmad r1, pa2, -sa3, c52  [rpt=4 moe]' ...   prints "<hi> <lo>"
  python3 asm.py --check                                          round-trips the corpora

Syntax is what dis.py prints (see hw.py): optional predicate in
parentheses, mnemonic with its .modifiers, operands in hardware slot order
(r o pa sa c g i # idx, bank[idxN+off] for indexed operands, -x / |x| for
modifiers, .f16.lo|.hi / .c10 under format select), then in brackets the flags and fields
(name or name=value). Every form comes from libusc's assembler, so anything
the encoder can't produce is rejected rather than guessed.

assemble() also takes the older fields.json syntax (asm_fz.py, e.g.
"pck.u8.f32 o0.f8.f9, c52, c48 [f1.0 f2.8 h12]") so existing callers such as
tools/decode/mkreplay.py keep working.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw  # noqa: E402

# An immediate written as a bare identifier: `#CR_3D_KICK`, resolved from a
# per-core symbol table (docs/UKERNEL-TOOLING.md §2/§4). `#0x428` / `#1064` are
# left alone, so numeric callers are unaffected.
_SYM = re.compile(r"#([A-Za-z_]\w*)\b")


def _resolve(line, symbols):
    def sub(m):
        name = m.group(1)
        if name not in symbols:
            raise ValueError(f"unknown symbol #{name}")
        return "#" + str(int(symbols[name]))
    return _SYM.sub(sub, line)


def assemble(line, symbols=None):
    """-> (hi, lo). With `symbols`, a per-core constants table (name -> int),
    any `#NAME` immediate is resolved from it first, so microkernel source can
    read `str #CR_3D_KICK, r5`. Expressions are not evaluated (a caller needing
    `CR_3D_KICK // 4` keeps the literal); `symbols=None` is the old behaviour."""
    if symbols:
        line = _resolve(line, symbols)
    try:
        lo, hi = hw.encode(hw.parse(line))
        return hi, lo
    except ValueError as e:
        try:
            import asm_fz
            return asm_fz.assemble(line)
        except Exception:
            raise e


def check():
    import hwcheck
    return hwcheck.main([])


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        sys.exit(check())
    for line in sys.argv[1:]:
        hi, lo = assemble(line)
        print(f"{hi:08x} {lo:08x}  {line}")
