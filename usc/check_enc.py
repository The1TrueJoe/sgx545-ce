"""Check the generated C encoder against hw.py: every instruction of the
corpora (re/isa/corpus/c1..c3), decoded by hw.decode(), is handed to
usse_encode() (tests/enc_lines.c) and must come back as the same 64 bits.

    python3 -I usc/check_enc.py [build/enc_lines]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ISA = os.path.join(HERE, "..", "re", "isa")
sys.path.insert(0, ISA)
import hw  # noqa: E402
import usp  # noqa: E402

KINDS = ["r", "o", "pa", "sa", "c", "g", "i", "#", "ix", "idx", "p"]
IXB = ["r", "o", "pa", "sa"]
FIELDS = sorted({fl.name for f in hw.FORMS for fl in f.fields})
FID = {n: i for i, n in enumerate(FIELDS)}
FORMI = {id(f): i for i, f in enumerate(hw.FORMS)}


def line(x, fc):
    ws = [FORMI[id(x.form)], fc, len(x.d)]
    for k, v in x.d.items():
        ws += [FID[k], v]
    ws.append(len(x.ops))
    for o in x.ops:
        k = o["kind"]
        if k not in KINDS:
            return None
        sfx = list(o.get("sfx", []))
        lane = swz = nw = c10 = 0
        for x in list(sfx):
            if len(x) == 2 and x[0] == "l" and x[1].isdigit():
                lane = int(x[1])
            elif len(x) == 4 and set(x) <= set("xyzw"):
                swz = 1
                n = sum("xyzw".index(ch) << (2 * i) for i, ch in enumerate(x))
            elif x == "nw":
                nw = 1
            elif x == "c10":
                c10 = 1
            else:
                return None
        f16 = 1 if o.get("f16") or o.get("fc") == "f16" else 0
        c10 = c10 or o.get("fc") == "c10"
        ws += [KINDS.index(k), n if swz else o.get("off", o.get("n", 0)), IXB.index(o["bank"]) if k == "ix" else 0,
               o.get("idx", 0), o.get("neg", 0), o.get("abs", 0), f16, o.get("half", 0),
               1 if c10 else 0, swz, nw, lane]
    return " ".join(map(str, ws))


def corpus_words():
    for c in ("c1", "c2", "c3"):
        for s in usp.corpus(os.path.join(ISA, "corpus", c)):
            for part in [s["pre"]] + [b["insts"] for b in s["blocks"] if "insts" in b]:
                hw.CTX["fc"] = 2
                for lo, hi in part:
                    x = hw.decode(lo, hi)
                    if x is not None:
                        yield lo, hi, x, hw.CTX["fc"]
                    hw.track(x)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "enc_lines")
    todo, skipped = [], 0
    for lo, hi, x, fc in corpus_words():
        t = line(x, fc)
        if t is None:
            skipped += 1
            continue
        todo.append((lo, hi, t, hw.fmt(x)))
    r = subprocess.run([exe], input="\n".join(t for _, _, t, _ in todo) + "\n", capture_output=True, text=True)
    got = r.stdout.split("\n")
    bad = 0
    for (lo, hi, t, txt), g in zip(todo, got):
        if g != f"{hi:08x} {lo:08x}":
            bad += 1
            if bad <= 20:
                print(f"MISMATCH {hi:08x} {lo:08x} {txt}: C gives {g}")
    print(f"{len(todo)} corpus instructions, {len(todo) - bad} encode identically in C, {bad} differ, "
          f"{skipped} skipped (operand kinds C doesn't model)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
