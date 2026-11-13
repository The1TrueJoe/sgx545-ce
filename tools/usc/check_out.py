"""Check every instruction sgxc emitted (the USP code blocks, branch blocks
and smp templates of OUTDIR/*.usp) two ways:

1. sgx-disasm (built from sgx.xml) decodes it whole: a form, no unused bit set;
2. its text assembles back through sgx-asm to the same 64 bits.

    python3 -I tools/usc/check_out.py OUTDIR

sgx-asm / sgx-disasm come from SGX_ASM or SGX_DISASM (`sh ukernel/sgx-asm.sh DIR`).
"""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "isa"))
sys.path.insert(0, HERE)
import asm  # noqa: E402
import sgx_usp  # noqa: E402

# sgx_usp.c's block format control (2: C10 on) -> sgx-asm/-disasm -F (bit 0 C10, bit 1 F16)
FC = {0: 0, 1: 2, 2: 1, 3: 3}


def words(path):
    """[(fc, [(lo, hi, where)])]: the runs of instructions to check, each
    disassembled from its own format control"""
    s = sgx_usp.load(path)
    out = []
    for b in s["blocks"]:
        if b["type"] == "code":
            out.append((FC[b["flags"] & 3], [(lo, hi, "code") for lo, hi in b["insts"]]))
        elif b["type"] == "branch":
            out.append((1, [(lo, hi, "branch") for lo, hi in b["insts"]]))
        elif b["type"] == "sample5" and b["dependent"]:
            lo, hi = b["smp"]
            out.append((1, [(lo, hi, "smp template")]))
    return out


def main():
    d = [a for a in sys.argv[1:] if not a.startswith("--")][0]
    n = bad = 0
    forms = {}
    for p in sorted(glob.glob(os.path.join(d, "*.usp"))):
        name = os.path.basename(p)
        for fc, run in words(p):
            ws = [(lo, hi) for lo, hi, _ in run]
            texts = asm.disasm(ws, fc=fc)
            good = [i for i, t in enumerate(texts) if asm.ok(t)]
            for i, t in enumerate(texts):
                n += 1
                if not asm.ok(t):
                    bad += 1
                    lo, hi, where = run[i]
                    print(f"{name}: {hi:08x} {lo:08x} ({where}): {t}")
            if not good:
                continue
            try:
                back = asm.assemble_lines([texts[i] for i in good], fc=fc)
            except ValueError as e:
                bad += len(good)
                print(f"{name}: does not assemble back: {e}")
                continue
            for i, w in zip(good, back):
                lo, hi, where = run[i]
                x = asm.parse(texts[i])
                forms[x.name] = forms.get(x.name, 0) + 1
                if w != (lo, hi):
                    bad += 1
                    print(f"{name}: {hi:08x} {lo:08x} {texts[i]}: assembles to {w[1]:08x} {w[0]:08x}")
    print(f"{n} instructions: {n - bad} decode whole and round-trip")
    print("forms: " + ", ".join(f"{k} {v}" for k, v in sorted(forms.items())))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
