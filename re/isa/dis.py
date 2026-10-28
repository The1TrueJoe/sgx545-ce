"""Disassemble SGX545 USSE code (DDK 1.7 encoding) from USP binaries.

  python3 dis.py <dir or .usp>...      every shader: secondary program, then main
  python3 dis.py -x <hi> <lo>...       instructions given as hex word pairs

Decoding comes from hw.py, whose forms are read from libusc's own assembler
(see ISA.md). Each instruction prints as

  <hi> <lo>  [(pred)] mnemonic[.mods] operands  [flags]  {?unexplained bits}

A word no form matches prints as hwNN? with all its set bits listed. The
{?...} list is empty for every instruction of the corpora (hwcheck.py)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw  # noqa: E402
import usp  # noqa: E402


def line(lo, hi):
    x = hw.decode(lo, hi)
    if x is None:
        bits = [hw.bname(b) for b in range(64) if (hi << 32 | lo) >> b & 1]
        return f"{hi:08x} {lo:08x}  hw{hi >> 27:02x}?  {{?{' '.join(bits)}}}"
    t = f"{hi:08x} {lo:08x}  {hw.fmt(x)}"
    if x.unknown:
        t += f"  {{?{' '.join(x.unknown)}}}"
    return t


def dis_program(insts, indent="    "):
    return [indent + line(lo, hi) for lo, hi in insts]


def sample_text(b):
    try:
        import smpblock  # the USP texture-sample block parser (notes_sample.md)
        return smpblock.describe(b)
    except Exception:
        return f"({b['type']} block: texture sample generated at finalize)"


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
            out.append("    " + sample_text(b))
    return out


def main(argv):
    if argv and argv[0] == "-x":
        w = argv[1:]
        for i in range(0, len(w), 2):
            print(line(int(w[i + 1], 16), int(w[i], 16)))
        return
    for a in argv:
        paths = [a] if a.endswith(".usp") else sorted(
            os.path.join(a, f) for f in os.listdir(a) if f.endswith(".usp"))
        for p in paths:
            print("\n".join(dis_shader(usp.load(p))))


if __name__ == "__main__":
    main(sys.argv[1:])
