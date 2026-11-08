"""Instruction counts, sgxc vs libusc, per shader.

    python3 -I tools/usc/count.py OURS_DIR DDK_DIR [--md]

Both directories hold NNN-name.usp (DDK_DIR: tools/isa/corpus/c1, or the
DDK binaries hwcmp.py saved). Counted: the main program's code and branch
instructions, its issue slots (a repeat counts its iterations), its
temporaries (header n_temps), the secondary update program's instructions
(per draw, not per pixel: shown apart), and the texture sample blocks (the
finalizer adds their code).
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "isa"))
import hw  # noqa: E402
import usp  # noqa: E402


def counts(path):
    """(main instructions, secondary instructions, samples, issue slots, temporaries)"""
    s = usp.load(path)
    main = slots = 0
    for b in s["blocks"]:
        if b["type"] not in ("code", "branch"):
            continue
        hw.CTX["fc"] = b.get("flags", 2)
        for lo, hi in b["insts"]:
            x = hw.decode(lo, hi)
            m = re.search(r"rpt=(\d+)", hw.fmt(x)) if x is not None else None
            hw.track(x)
            main += 1
            slots += int(m.group(1)) if m else 1
    smp = sum(1 for b in s["blocks"] if b["type"] == "sample5")
    return main, len(s["pre"]), smp, slots, s["hdr"]["n_temps"]


def by_name(d):
    out = {}
    for p in sorted(glob.glob(os.path.join(d, "*.usp"))):
        b = os.path.basename(p)[:-4]
        name = b.split("-", 1)[1] if b[:3].isdigit() else b.split(".")[0]
        out[name] = p
    return out


def main():
    md = "--md" in sys.argv
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    ours, ddk = by_name(a[0]), by_name(a[1])
    rows, tot = [], [0] * 8
    for n in ours:
        if n not in ddk:
            continue
        o, d = counts(ours[n]), counts(ddk[n])
        rows.append((n, o, d))
        for i, v in enumerate((o[0], d[0], d[1], o[3], d[3], o[4], d[4], 1)):
            tot[i] += v
    if md:
        print("| shader | sgxc | libusc main | libusc secondary | slots | temps | samples |")
        print("|---|---|---|---|---|---|---|")
        for n, o, d in rows:
            print(f"| {n} | {o[0]} | {d[0]} | {d[1]} | {o[3]} / {d[3]} | {o[4]} / {d[4]} | {o[2]} / {d[2]} |")
        print(f"| **total ({tot[7]})** | **{tot[0]}** | **{tot[1]}** | **{tot[2]}** | **{tot[3]} / {tot[4]}** "
              f"| **{tot[5]} / {tot[6]}** | |")
    else:
        for n, o, d in rows:
            print(f"{n:16} sgxc {o[0]:3}   libusc {d[0]:3} + {d[1]:3} secondary   slots {o[3]}/{d[3]}"
                  f"   temps {o[4]}/{d[4]}   samples {o[2]}/{d[2]}")
        print(f"total over {tot[7]}: sgxc {tot[0]}, libusc {tot[1]} main + {tot[2]} secondary; "
              f"slots {tot[3]}/{tot[4]}, temps {tot[5]}/{tot[6]}")


if __name__ == "__main__":
    main()
