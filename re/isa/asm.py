"""Assemble SGX545 USSE instructions: the inverse of dis.py.

  python3 asm.py '(p0) fmad r1, pa2, -sa3, c52  [rpt=4 moe]' ...   prints "<hi> <lo>"
  python3 asm.py --check                                          round-trips the corpora

Syntax is what dis.py prints (see hw.py): optional predicate in
parentheses, mnemonic with its .modifiers, operands in hardware slot order
(r o pa sa c g i # idx, bank[idxN+off] for indexed operands, -x / |x| for
modifiers, .f16 under format select), then in brackets the flags and fields
(name or name=value). Every form comes from libusc's assembler, so anything
the encoder can't produce is rejected rather than guessed.

assemble() also takes the older fields.json syntax (asm_fz.py, e.g.
"pck.u8.f32 o0.f8.f9, c52, c48 [f1.0 f2.8 h12]") so existing callers such as
re/decode/mkreplay.py keep working.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw  # noqa: E402


def assemble(line):
    """-> (hi, lo)"""
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
