"""Check the compiler's encoder (mesa/src/sgx/isa, isaspec) against hw.py:
every instruction of the corpora (tools/isa/corpus/c1..c3), decoded by
hw.decode() and printed in dis.py's notation, is assembled and encoded by
sgx_isa_test -t and must come back as the same 64 bits.

    mesa/tests/isa-standalone/standalone.sh <mesa-tree>    (or a Mesa build's sgx_isa_test)
    python3 -I tools/usc/check_enc.py [mesa/src/sgx/isa/tests/build/sgx_isa_test]
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ISA = os.path.join(HERE, "..", "isa")
sys.path.insert(0, ISA)
import hw  # noqa: E402
import usp  # noqa: E402

START = "setfc #0, #1"   # program-start format control (hw.CTX["fc"] = 2)


def norm(t):
    return re.sub(r"\s+", " ", t.strip())


def compact(t):
    """the same instruction in the spellings ISA.md lists as equivalent and
    the isaspec assembler takes: efo's i=/a=/m= as x/y, fdual's neg1/neg2 as
    -src1/-src2 ("Other spellings asm.py takes")"""
    for f in "iam":
        t = re.sub(r"\b%s=%s0=([^,\s\]]+),%s1=([^\s\]]+)" % (f, f if f != "a" else "a", f if f != "a" else "a"),
                   lambda m, f=f: f"{f}={m.group(1)}/{m.group(2)}", t)
    m = re.match(r"^(\([^)]*\) )?(fdual\.\S+) (.*?)(\s+\[(.*)\])?$", t)
    if m and m.group(5) and re.search(r"\bneg[12]\b", m.group(5)):
        ops = [o.strip() for o in m.group(3).split(",")]
        items = m.group(5).split()
        for j, flag in ((2, "neg1"), (3, "neg2")):
            if flag in items:
                items.remove(flag)
                ops[j] = ops[j][1:] if ops[j].startswith("-") else "-" + ops[j]
        t = (m.group(1) or "") + m.group(2) + " " + ", ".join(ops) + (f"  [{' '.join(items)}]" if items else "")
    return t


def corpus_words():
    for c in ("c1", "c2", "c3"):
        for s in usp.corpus(os.path.join(ISA, "corpus", c)):
            for part in [s["pre"]] + [b["insts"] for b in s["blocks"] if "insts" in b]:
                hw.CTX["fc"] = 2
                yield None
                for lo, hi in part:
                    x = hw.decode(lo, hi)
                    if x is not None:
                        yield lo, hi, hw.fmt(x)
                    hw.track(x)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        HERE, "..", "..", "mesa", "src", "sgx", "isa", "tests", "build", "sgx_isa_test")
    lines, todo, plain = [], [], []
    for w in corpus_words():
        if w is None:
            lines.append(START)
            plain.append(START)
        else:
            lines.append(compact(w[2]))
            plain.append(w[2])
            todo.append((w[0], w[1], compact(w[2])))
    # (dis.py's own spellings: ISA.md requires the assembler to take them too)
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(plain) + "\n")
    try:
        r0 = subprocess.run([exe, "-t", f.name], capture_output=True, text=True)
    finally:
        os.unlink(f.name)
    n_plain = sum(1 for ln in r0.stdout.split("\n") if " ; " in ln and norm(ln.rsplit(" ; ", 1)[0]) != START)
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(lines) + "\n")
    try:
        r = subprocess.run([exe, "-t", f.name], capture_output=True, text=True)
    finally:
        os.unlink(f.name)
    got = []
    for ln in r.stdout.split("\n"):
        if ln.startswith("#") or " ; " not in ln:
            continue
        t, w = ln.rsplit(" ; ", 1)
        if norm(t) != START:
            got.append((norm(t), w.split()[:2]))
    # the table leaves out lines it can't assemble: match in order by text
    bad = rejected = 0
    k = 0
    for lo, hi, txt in todo:
        if k < len(got) and got[k][0] == norm(txt):
            glo, ghi = got[k][1]
            k += 1
            if (int(glo, 16), int(ghi, 16)) != (lo, hi):
                bad += 1
                if bad <= 20:
                    print(f"MISMATCH {hi:08x} {lo:08x} {txt}: isaspec gives {ghi} {glo}")
        else:
            rejected += 1
            if rejected <= 20:
                print(f"REJECTED {hi:08x} {lo:08x} {txt}")
    print(f"{len(todo)} corpus instructions, {len(todo) - bad - rejected} encode identically, {bad} differ, "
          f"{rejected} not assembled; {len(todo) - n_plain} need the compact spelling (efo i=/a=/m=, fdual neg1/neg2)")
    sys.exit(1 if bad or rejected else 0)


if __name__ == "__main__":
    main()
