"""Instruction counts, sgxc vs libusc, per shader.

    python3 -I usc/count.py OURS_DIR DDK_DIR [--md]

Both directories hold NNN-name.usp (DDK_DIR: re/isa/corpus/c1, or the
DDK binaries hwcmp.py saved). Counted: the main program's code and branch
instructions, the secondary update program's (per draw, not per pixel:
shown apart), and the texture sample blocks (the finalizer adds their code).
"""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "re", "isa"))
import usp  # noqa: E402


def counts(path):
    s = usp.load(path)
    main = sum(len(b["insts"]) for b in s["blocks"] if b["type"] in ("code", "branch"))
    smp = sum(1 for b in s["blocks"] if b["type"] == "sample5")
    return main, len(s["pre"]), smp


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
    rows, tot = [], [0, 0, 0, 0]
    for n in ours:
        if n not in ddk:
            continue
        o, d = counts(ours[n]), counts(ddk[n])
        rows.append((n, o, d))
        tot[0] += o[0]
        tot[1] += d[0]
        tot[2] += d[1]
        tot[3] += 1
    if md:
        print("| shader | sgxc | libusc main | libusc secondary | samples |")
        print("|---|---|---|---|---|")
        for n, o, d in rows:
            print(f"| {n} | {o[0]} | {d[0]} | {d[1]} | {o[2]} / {d[2]} |")
        print(f"| **total ({tot[3]})** | **{tot[0]}** | **{tot[1]}** | **{tot[2]}** | |")
    else:
        for n, o, d in rows:
            print(f"{n:16} sgxc {o[0]:3}   libusc {d[0]:3} + {d[1]:3} secondary   samples {o[2]}/{d[2]}")
        print(f"total over {tot[3]}: sgxc {tot[0]}, libusc {tot[1]} main + {tot[2]} secondary")


if __name__ == "__main__":
    main()
