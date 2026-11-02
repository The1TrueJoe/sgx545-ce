"""Spec-side black-box check of an encoder written from docs/ISA.md alone
(docs/CLEANROOM.md "Validation").

    python3 -I tools/isa/blind.py gen SEED OUT.json       one random valid instruction per form
    python3 -I tools/isa/blind.py text KEY.json           the instruction texts, numbered (hand these over)
    python3 -I tools/isa/blind.py score KEY.json WORDS    compare "n hi lo ..." lines with the key
    python3 -I tools/isa/blind.py table FILE...           check "<text> ; <lo> <hi>" lines written by
                                                          the implementation side (setfc lines update
                                                          format control; # starts a comment)

gen draws random words per form, clears the bits the form doesn't use, and
keeps a word only if it decodes to that form and its text re-encodes to the
same bits (format control at its program-start state). score prints, per
wrong word, which bits differ and which ISA.md field they belong to: field
names and bit positions only, which is what may go back to the
implementation side. Never hand over the key or this file's tables.
"""
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hw  # noqa: E402


def gen(seed, path):
    rnd = random.Random(seed)
    out = []
    for f in hw.FORMS:
        cls = [v for s, v in f.fixed if s == "h27-31"][0]
        for _ in range(4000):
            v = rnd.getrandbits(64) & (rnd.getrandbits(64) | rnd.getrandbits(64))
            v = (v & ~f.fmask) | f.fval
            hw.CTX["fc"] = 2
            try:
                ins = hw.decode(v & 0xFFFFFFFF, v >> 32)
                if ins is not None and ins.form is f and ins.unknown:
                    for b in ins.unknown:
                        v &= ~(1 << (int(b[1:]) + (32 if b[0] == "h" else 0)))
                    ins = hw.decode(v & 0xFFFFFFFF, v >> 32)
                if ins is None or ins.form is not f or ins.unknown:
                    continue
                t = hw.fmt(ins)
                lo, hi = hw.encode(hw.parse(t))
                if "?" in t or (hi << 32 | lo) != v:
                    continue
            except Exception:
                continue
            out.append({"hw": cls, "text": t, "hi": f"{v >> 32:08x}", "lo": f"{v & 0xFFFFFFFF:08x}"})
            break
        else:
            print("no sample for", f.name, file=sys.stderr)
    json.dump(out, open(path, "w"), indent=0)
    print(f"{len(out)} instructions")


def text(path):
    for n, k in enumerate(json.load(open(path)), 1):
        print(f"{n}\t{k['text']}")


def diff_text(v, w):
    """the bits of w that differ from the expected word v, and their ISA.md fields"""
    diff = v ^ w
    f = next(x for x in hw.FORMS if x.matches(v))
    names = [fl.name for fl in f.fields if hw.mask(fl.spec) & diff]
    for o in f.ops:
        for part in ("num", "bank", "ext", "neg", "abs"):
            spec = getattr(o, part)
            if spec and hw.mask(spec) & diff:
                names.append(f"{o.role} {part}")
    bits = " ".join(hw.bname(b) for b in range(64) if diff >> b & 1)
    return f"bits {bits} [{', '.join(dict.fromkeys(names))}]"


def table(paths):
    ok = bad = rej = 0
    for path in paths:
        hw.CTX["fc"] = 2
        for ln, line in enumerate(open(path), 1):
            if line.lstrip().startswith("#") or ";" not in line:
                continue
            t, _, w = line.partition(";")
            w = w.split()
            got = int(w[1], 16) << 32 | int(w[0], 16)
            try:
                lo, hi = hw.encode(hw.parse(t))
            except Exception as e:
                rej += 1
                print(f"{path}:{ln} rejected {t.strip()!r}: {e}")
                continue
            hw.track(hw.decode(lo, hi))
            v = hi << 32 | lo
            if v == got:
                ok += 1
                continue
            bad += 1
            print(f"{path}:{ln} {t.strip()[:60]:60} {diff_text(v, got)}")
    print(f"{ok} exact, {bad} differ, {rej} rejected")


def score(path, words):
    key = json.load(open(path))
    got = {}
    for line in open(words):
        p = line.split()
        if len(p) >= 3 and p[0].isdigit():
            got[int(p[0])] = int(p[1], 16) << 32 | int(p[2], 16)
    ok = 0
    for n, k in enumerate(key, 1):
        v = int(k["hi"], 16) << 32 | int(k["lo"], 16)
        if got.get(n) == v:
            ok += 1
            continue
        if n not in got:
            print(f"{n:3} missing")
            continue
        print(f"{n:3} {k['text'][:60]:60} {diff_text(v, got[n])}")
    print(f"{ok}/{len(key)} exact")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["gen"]:
        gen(int(a[1]), a[2])
    elif a[:1] == ["text"]:
        text(a[1])
    elif a[:1] == ["score"]:
        score(a[1], a[2])
    elif a[:1] == ["table"]:
        table(a[1:])
    else:
        sys.exit(__doc__)
