"""USSE text <-> words through the sgx-asm / sgx-disasm host tools (built from
mesa/src/sgx/isa/sgx.xml; `sh ukernel/sgx-asm.sh DIR` builds both).

  python3 asm.py '(p0) fmad r1, pa2, -sa3, c52 [rpt=4]' ...   prints "<hi> <lo>  <text>"
  python3 asm.py -d LO HI ...                                prints the disassembly

The tools are found through SGX_ASM / SGX_DISASM (either one finds its
sibling), else on PATH.

  assemble(line) -> (hi, lo)            one instruction
  assemble_lines(lines) -> [(lo, hi)]   a program, format control tracked across it
  disasm(words) -> [text]               words: [(lo, hi)] or little-endian bytes
  parse(text) -> Inst                   the text split into predicate, mnemonic,
                                        operands and [flags] (no ISA knowledge)
"""
import os
import re
import shutil
import struct
import subprocess
import sys

# An immediate written as a bare identifier: `#CR_3D_KICK`, resolved from a
# per-core symbol table (docs/UKERNEL-TOOLING.md §2/§4). `#0x428` / `#1064` are
# left alone, so numeric callers are unaffected.
_SYM = re.compile(r"#([A-Za-z_]\w*)\b")

_HINT = "build them with `sh ukernel/sgx-asm.sh DIR` and set SGX_ASM=DIR/sgx-asm"


def tool(name):
    """the path of sgx-asm or sgx-disasm"""
    env = {"sgx-asm": "SGX_ASM", "sgx-disasm": "SGX_DISASM"}
    p = os.environ.get(env[name])
    if p:
        return p
    for other in env.values():
        q = os.environ.get(other)
        if q:
            sib = os.path.join(os.path.dirname(q), name)
            if os.path.exists(sib):
                return sib
    p = shutil.which(name)
    if p:
        return p
    raise RuntimeError(f"{name} not found: {_HINT}")


def _resolve(line, symbols):
    def sub(m):
        name = m.group(1)
        if name not in symbols:
            raise ValueError(f"unknown symbol #{name}")
        return "#" + str(int(symbols[name]))
    return _SYM.sub(sub, line)


def _fc_args(fc):
    return [] if fc is None else ["-F", str(fc)]


def assemble_lines(lines, fc=None):
    """-> [(lo, hi)], one per line. fc: sgx-asm's -F (bit 0 C10, bit 1 F16;
    default the program-start state). Raises ValueError on any error."""
    lines = list(lines)
    if not lines:
        return []
    for ln in lines:
        if "\n" in ln or not ln.strip() or ln.lstrip()[0] in "#;":
            raise ValueError(f"not one instruction: {ln!r}")
    p = subprocess.run([tool("sgx-asm"), "-b"] + _fc_args(fc), input=("\n".join(lines) + "\n").encode(),
                       capture_output=True)
    if p.returncode or len(p.stdout) != 8 * len(lines):
        err = [ln for ln in p.stderr.decode(errors="replace").splitlines() if not ln.startswith("Unhandled")]
        raise ValueError("\n".join(err).strip() or "sgx-asm failed")
    return [struct.unpack_from("<II", p.stdout, 8 * i) for i in range(len(lines))]


def assemble(line, symbols=None):
    """-> (hi, lo). With `symbols`, a per-core constants table (name -> int),
    any `#NAME` immediate is resolved from it first."""
    if symbols:
        line = _resolve(line, symbols)
    (lo, hi), = assemble_lines([line])
    return hi, lo


def disasm(words, fc=None):
    """-> one text per instruction (setfc tracked across them). A word that
    doesn't decode gets sgx-disasm's error text; ok() tells them apart."""
    if not isinstance(words, (bytes, bytearray)):
        words = b"".join(struct.pack("<II", lo, hi) for lo, hi in words)
    if len(words) % 8:
        raise ValueError("words: not a whole number of instructions")
    if not words:
        return []
    p = subprocess.run([tool("sgx-disasm")] + _fc_args(fc), input=bytes(words), capture_output=True)
    out = p.stdout.decode(errors="replace").split("\n")[:-1]
    if len(out) != len(words) // 8:
        raise RuntimeError(f"sgx-disasm: {len(out)} lines for {len(words) // 8} words: "
                           + p.stderr.decode(errors="replace"))
    return out


def ok(text):
    """did sgx-disasm decode the word whole?"""
    return not any(s in text for s in ("no match", "unexpected bits", "dontcare"))


def matched(text):
    """did the word match a form at all (perhaps with bits the form leaves unused)?"""
    return not text.startswith("no match")


def body(text):
    """the instruction text without sgx-disasm's trailing warnings"""
    return text.split(" ; WARNING", 1)[0].rstrip()


# --------------------------------------------------------------------------
# text

class Form:
    def __init__(self, name):
        self.name = name


class Inst:
    """pred: '' or 'p0' / '!p1' / 'pn'; name: the base mnemonic; mods: the
    .suffixes; ops: operand dicts (kind, n | bank/idx/off, neg, abs, sfx);
    flags: {name: value}, a bare flag is 1, a number an int, else the text."""

    def __init__(self, text, pred, name, mods, ops, flags):
        self.text, self.pred, self.name, self.mods = text, pred, name, mods
        self.form = Form(name)
        self.ops, self.flags = ops, flags


def opd(t):
    """one operand: r5, pa2, sa[idx1+3], #-4, c52, g17, i0, idx1, p0, -x, |x|, x.f16 ..."""
    o = {}
    t = t.strip()
    if t.startswith("-"):
        o["neg"] = 1
        t = t[1:]
    if t.startswith("|") and t.endswith("|"):
        o["abs"] = 1
        t = t[1:-1]
    m = re.fullmatch(r"(r|o|pa|sa)\[idx(\d)\+(\d+)\]((?:\.\w+)*)", t)
    if m:
        o.update(kind="ix", bank=m.group(1), idx=int(m.group(2)), off=int(m.group(3)))
        sfx = m.group(4)
    else:
        m = re.fullmatch(r"([a-z]+|#)(-?\d+|-?0x[0-9a-fA-F]+)((?:\.\w+)*)", t)
        if not m:
            raise ValueError(f"bad operand {t!r}")
        o.update(kind=m.group(1), n=int(m.group(2), 0))
        sfx = m.group(3)
    if sfx:
        o["sfx"] = sfx[1:].split(".")
    return o


def _split_ops(s):
    out, cur, depth = [], "", 0
    for ch in s:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def parse(text):
    t = text.strip()
    pred = ""
    m = re.match(r"\((!?p[0-3n])\)\s*", t)
    if m:
        pred = m.group(1)
        t = t[m.end():]
    flags = {}
    m = re.search(r"\s\[([^\[\]]*)\]\s*$", t)       # trailing " [flags]" (an index has no space before '[')
    if m:
        for x in m.group(1).split():
            k, eq, v = x.partition("=")
            if not eq:
                flags[k] = 1
            else:
                try:
                    flags[k] = int(v, 0)
                except ValueError:
                    flags[k] = v
        t = t[:m.start()]
    mnem, _, rest = t.strip().partition(" ")
    parts = mnem.split(".")
    ops = [opd(x) for x in _split_ops(rest)] if rest.strip() else []
    return Inst(text, pred, parts[0], parts[1:], ops, flags)


def main(argv):
    if argv[:1] == ["-d"]:
        w = [int(x, 16) for x in argv[1:]]
        for t in disasm(list(zip(w[0::2], w[1::2]))):
            print(t)
        return 0
    words = assemble_lines(argv)
    for (lo, hi), line in zip(words, argv):
        print(f"{hi:08x} {lo:08x}  {line}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (ValueError, RuntimeError) as e:
        sys.exit(f"asm.py: {e}")
