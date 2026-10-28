"""Disassemble SGX545 USSE instructions (DDK 1.7 encoding), from USP binaries.

  python3 dis.py <dir or .usp>...      every shader: secondary program, then main
  python3 dis.py -x <hi> <lo>          one instruction

Fields come from ISA.md. Each instruction prints as

  <hi> <lo>  <mnemonic> <operands>  [flags]  {?unknown bits}

Operands are in USEASM argument order (the first is the destination for ALU
ops). Every 1-bit no field explains is listed in {?...} by position (l = low
word, h = high word), so nothing is silently dropped. A mnemonic that isn't
backed by a one-op corpus shader carries a trailing '?'; flags print as the
encoder input that sets them (f1.13 = USEASM flags1 bit 13) unless named."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usp  # noqa: E402
from isa import decode  # noqa: E402


def line(hi, lo):
    text, unknown = decode(hi, lo)
    return f"{hi:08x} {lo:08x}  {text}" + (f"  {{?{' '.join(unknown)}}}" if unknown else "")


def dis_program(insts, indent="    "):
    return [indent + line(hi, lo) for lo, hi in insts]


def dis_shader(s):
    out = [f"== {s['name']} ({s['kind']})"]
    if s["pre"]:
        out.append("  secondary update program:")
        out += dis_program(s["pre"])
    out.append("  main program:")
    for b in s["blocks"]:
        if b["type"] == "label":
            out.append(f"  L{b['label']}:")
        elif b["type"] == "code":
            out += dis_program(b["insts"])
        elif b["type"] == "branch":
            out += [x + f"   -> L{b['label']}" for x in dis_program(b["insts"])]
        else:
            out.append(f"    ({b['type']} block: texture sample generated at finalize)")
    return out


def main(argv):
    if argv and argv[0] == "-x":
        hi, lo = int(argv[1], 16), int(argv[2], 16)
        print(line(hi, lo))
        return
    for a in argv:
        paths = [a] if a.endswith(".usp") else sorted(
            os.path.join(a, f) for f in os.listdir(a) if f.endswith(".usp"))
        for p in paths:
            print("\n".join(dis_shader(usp.load(p))))


if __name__ == "__main__":
    main(sys.argv[1:])
