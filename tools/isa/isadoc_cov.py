"""Spec-side check: is docs/ISA.md complete enough to write the encoder from?

For every form in hw.py it asks whether ISA.md, in the sections for the
form's hardware class (plus the sections every class shares), names the
form, states each identifying bit range, each field's bit position and each
operand slot's number/bank/extension/modifier bits. A position counts as
stated when some bit-range token in those sections (h9-10, l21-27,
h12,h13,h14,h11, h13 + h18 ...) covers exactly the same bits. That is a necessary
condition, not a proof: it can't tell whether the prose around the token is
right. The point is to list what an implementer reading only ISA.md could
not find.

    python3 -I tools/isa/isadoc_cov.py [-v]

Spec-side tooling (docs/CLEANROOM.md): it reads hw.py, so the implementation
side doesn't run it.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hw  # noqa: E402

DOC = os.path.join(HERE, "..", "..", "docs", "ISA.md")
SHARED = ("Common fields", "Operands (generic", "Registers", "Shared ALU control bits",
          "Fields shared by hw 10-19")

TOK = re.compile(r"\b([lh]\d{1,2}(?:-[lh]?\d{1,2})?(?:\s*[,+]\s*[lh]\d{1,2}(?:-[lh]?\d{1,2})?)*)\b")


def bits(tok):
    out = set()
    for part in re.split(r"\s*[,+]\s*", tok):
        w = 0 if part[0] == "l" else 32
        if "-" in part:
            a, b = part[1:].split("-")
            a, b = int(a), int(b.lstrip("lh"))
            if b < a:
                return None
            out |= {w + i for i in range(a, b + 1)}
        else:
            out.add(w + int(part[1:]))
    return frozenset(out)


def sections(text):
    """[(level, title, body)] with each body including its subsections"""
    heads = [(m.start(), len(m.group(1)), m.group(2)) for m in re.finditer(r"^(#+) (.*)$", text, re.M)]
    out = []
    for i, (pos, lvl, title) in enumerate(heads):
        end = len(text)
        for pos2, lvl2, _ in heads[i + 1:]:
            if lvl2 <= lvl:
                end = pos2
                break
        out.append((lvl, title, text[pos:end]))
    return out


def classes_of(title):
    m = re.search(r"\bhw ((?:[0-9a-f]{1,2}(?:-[0-9a-f]{1,2})?(?:, )?)+)", title)
    if not m:
        return set()
    out = set()
    for part in m.group(1).split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = (int(x, 16) for x in part.split("-"))
            out |= set(range(a, b + 1))
        else:
            out.add(int(part, 16))
    return out


def hwclass(f):
    for spec, val in f.fixed:
        if spec == "h27-31":
            return val
    return None


def main():
    verbose = "-v" in sys.argv
    text = open(DOC).read()
    secs = sections(text)
    shared = "".join(b for _, t, b in secs if t.startswith(SHARED))
    by_class = {}
    for _, title, body in secs:
        for c in classes_of(title):
            by_class.setdefault(c, []).append(body)
    tokset = {}

    def toks(c):
        if c not in tokset:
            body = shared + "".join(by_class.get(c, []))
            tokset[c] = ({bits(t) for t in TOK.findall(body)} - {None}, body)
        return tokset[c]

    total = missing = 0
    nameless = []
    gaps = {}
    for f in hw.FORMS:
        c = hwclass(f)
        have, body = toks(c)
        if not re.search(r"(?<![\w.])" + re.escape(f.name) + r"(?![\w])", body):
            nameless.append(f"{f.name} (hw {c:02x})" if c is not None else f.name)
        want = [("fixed " + s, s) for s, _ in f.fixed]
        want += [(fl.name, fl.spec) for fl in f.fields]
        for o in f.ops:
            if o.text:
                continue
            for part in ("num", "bank", "ext", "neg", "abs"):
                spec = getattr(o, part)
                if spec:
                    want.append((f"{o.role}.{part}", spec))
        for what, spec in want:
            total += 1
            if frozenset(hw.bitlist(spec)) not in have:
                missing += 1
                gaps.setdefault(f.name, []).append(f"{what} {spec}")
        for fl in f.fields:
            if fl.kind != "enum" or not isinstance(fl.table, dict):
                continue
            for val, nm in fl.table.items():
                if not nm or re.fullmatch(r"(?:[a-z]+\d|alu[0-9a-f]{2}|i8op\d|s\dz\d[ao]|1-.*|.*=.*)", nm):
                    continue        # '' prints nothing; x7, ret3, alu1e ... are placeholder names;
                    #                 s1z0o is a rule (sign/zero/combine), 1-x is the complement bit,
                    #                 efo's a0=m0+m1 is written with spaces
                total += 1
                if not re.search(r"(?<![\w-])" + re.escape(nm) + r"(?![\w])", body):
                    missing += 1
                    gaps.setdefault(f.name, []).append(f"{fl.name}={val} '{nm}'")
    print(f"{len(hw.FORMS)} forms, {total} positions and enum values; {missing} not stated in ISA.md "
          f"({len(gaps)} forms); {len(nameless)} forms not named in their class's section")
    if nameless:
        print("not named:", " ".join(nameless))
    for name, g in gaps.items():
        if verbose or len(g) <= 4:
            print(f"  {name}: {'; '.join(g)}")
        else:
            print(f"  {name}: {'; '.join(g[:4])}; ... (+{len(g) - 4})")


if __name__ == "__main__":
    main()
