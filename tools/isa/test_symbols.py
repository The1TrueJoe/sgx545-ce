# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The symbolic assembler resolves per-core constant names to the same bytes as
the literals (docs/UKERNEL-TOOLING.md §6 step 4). Numeric callers are
unaffected, and an unknown symbol is rejected, not silently emitted.

    python3 tools/isa/test_symbols.py
"""
import importlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                                       # asm, hw
sys.path.insert(0, os.path.join(HERE, "..", "..", "ukernel"))  # cores package
import asm  # noqa: E402

core = importlib.import_module("cores.sgx545")
SYMS = {k: v for k, v in vars(core).items()
        if not k.startswith("_") and isinstance(v, int)}

# (symbolic form, the literal it must equal). Real microkernel operand shapes.
CASES = [
    ("st.a.d r17, #SC_INFLIGHT, r2 [moe]", "st.a.d r17, #%d, r2 [moe]" % SYMS["SC_INFLIGHT"]),
    ("ld.a.d r2, r17, #SC_CMD [moe]",       "ld.a.d r2, r17, #%d [moe]" % SYMS["SC_CMD"]),
    ("ld.a.d r2, r17, #SC_RD [moe]",        "ld.a.d r2, r17, #%d [moe]" % SYMS["SC_RD"]),
]


def main():
    bad = 0
    for sym_line, lit_line in CASES:
        if asm.assemble(sym_line, SYMS) != asm.assemble(lit_line):
            print(f"MISMATCH {sym_line!r} != {lit_line!r}")
            bad += 1
    try:
        asm.assemble("ld.a.d r2, r17, #NOT_A_SYMBOL [moe]", SYMS)
        print("MISSING: unknown symbol was not rejected")
        bad += 1
    except ValueError:
        pass
    # a numeric immediate is untouched even when a symbol table is supplied
    if asm.assemble("st.a.d r17, #0x14, r2 [moe]", SYMS) != \
       asm.assemble("st.a.d r17, #0x14, r2 [moe]"):
        print("MISMATCH: numeric immediate changed under symbols")
        bad += 1
    print("ok" if not bad else f"{bad} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
