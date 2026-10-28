"""Find out, on the EA3, which DDK constant number each uniform component
gets (libglslcompiler chooses them, and keeps the map outside the USP).

    python3 -I usc/hw/calib.py SGXC OUTDIR [-k name]

For every test in OUTDIR (a run_corpus.py output) whose shader under test
has uniforms: draw the real program (DDK-compiled, so the DDK's numbering
applies) with every uniform component set to a marker value code/255, but
with a probe shader of ours substituted that outputs constants k..k+3 as its
colour. Two passes, with codes 1.. and 101..: a constant that holds a
uniform component reads code + 100 in the second pass, a literal reads the
same in both. Vertex shaders are probed with a vertex shader that writes
constants k..k+3 into the real shader's first vec4 varying and a fragment
shader that outputs that varying.

Writes OUTDIR/calib/<name>.json: {"u": [k of each component], ...} for
run_corpus.py --calib.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import hwcmp  # noqa: E402
import layout  # noqa: E402

SIZES = {"float": 1, "vec2": 2, "vec3": 3, "vec4": 4, "mat2": 4, "mat3": 9, "mat4": 16}
STRUCT = re.compile(r"struct\s+(\w+)\s*\{([^}]*)\}\s*;", re.S)
MEMBER = re.compile(r"(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*;")
UNIF = re.compile(r"\buniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;")
VARY = re.compile(r"\bvarying\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*;")
ATTR = re.compile(r"\battribute\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*;")


def leaves(src):
    """[(GL name, type)] of the non-sampler uniforms"""
    structs = {m.group(1): MEMBER.findall(m.group(2)) for m in STRUCT.finditer(src)}
    out = []
    for ty, name, n in UNIF.findall(src):
        names = [f"{name}[{i}]" for i in range(int(n))] if n else [name]
        for nm in names:
            if ty in structs:
                out += [(f"{nm}.{mn}", mt) for mt, mn in structs[ty] if mt in SIZES]
            elif ty in SIZES:
                out.append((nm, ty))
    return out


def decls(src):
    """the shader without its main()"""
    i = src.find("void main")
    return src[:i] if i >= 0 else src


def markers(lv, base):
    """per leaf the values to set and the code of each component"""
    vals, codes, c = {}, {}, base
    for nm, ty in lv:
        n = SIZES[ty]
        codes[nm] = list(range(c, c + n))
        vals[nm] = [x / 255.0 for x in codes[nm]]
        c += n
    return vals, codes


def block(name, vs, fs, vals, lv, given=None):
    """a probe draw: the test's own state (given: its tests.list lines, for
    the other stage's uniforms, attributes, textures), the probed uniforms
    set to markers"""
    lines = [f"test {name}", f"pos {ATTR.findall(vs)[0][1]}", f"vs {hwcmp.BOX}/src/{name}.vert",
             f"fs {hwcmp.BOX}/src/{name}.frag"]
    probed = {nm for nm, _ in lv}
    if given:
        for ln in given:
            w = ln.split()
            if w and w[0] in ("u", "m", "i", "attr", "tex") and not (w[0] in ("u", "m") and w[1] in probed):
                lines.append(ln)
    else:
        for a, (_, an) in enumerate(ATTR.findall(vs)[1:]):
            lines.append(f"attr {an} {a + 1} " + " ".join(map(str, hwcmp.A1)))
    for nm, ty in lv:
        k = "m" if ty == "mat4" else "u"
        lines.append(f"{k} {nm} " + " ".join(f"{v:.9g}" for v in vals[nm]))
    lines.append("end")
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sgxc")
    ap.add_argument("outdir")
    ap.add_argument("-k")
    a = ap.parse_args()
    stage = os.path.join(a.outdir, "calib", "stage")
    shutil.rmtree(stage, ignore_errors=True)
    for d in ("src", "sub", "probe"):
        os.makedirs(os.path.join(stage, d))
    lines, plan = [], []
    given, cur = {}, None
    tl = os.path.join(a.outdir, "tests.list")
    for ln in (open(tl).read().splitlines() if os.path.exists(tl) else []):
        if ln.startswith("test "):
            cur = ln.split()[1]
            given[cur] = []
        elif cur:
            given[cur].append(ln)
    for usp in sorted(glob.glob(os.path.join(a.outdir, "*.usp"))):
        name = os.path.basename(usp)[4:-4]
        if a.k and a.k not in name:
            continue
        vs = open(os.path.join(a.outdir, "work", name + ".vert")).read()
        fs = open(os.path.join(a.outdir, "work", name + ".frag")).read()
        kind = "vert" if open(usp, "rb").read()[0x10] == 1 else "frag"
        src = vs if kind == "vert" else fs
        lv = leaves(src)
        if not lv:
            continue
        ncomp = sum(SIZES[t] for _, t in lv)
        maxk = 4 * ((ncomp + 3) // 4) + 16
        for k in range(0, maxk, 4):
            pdir = os.path.join(stage, "probe", f"{name}_{k}")
            os.makedirs(pdir)
            if kind == "frag":
                pv, pf = vs, "precision highp float;\nuniform vec4 __p;\nvoid main() { gl_FragColor = __p; }\n"
            else:
                vy = [n for t, n in VARY.findall(vs) if t == "vec4"]
                pos = ATTR.findall(vs)[0][1]
                if not vy:
                    print(f"{name}: no vec4 varying to probe through")
                    break
                pv = decls(vs) + f"uniform vec4 __p;\nvoid main() {{ gl_Position = {pos}; {vy[0]} = __p; }}\n"
                pf = decls(fs) + f"void main() {{ gl_FragColor = {vy[0]}; }}\n"
            open(os.path.join(pdir, "p.vert"), "w").write(pv)
            open(os.path.join(pdir, "p.frag"), "w").write(pf)
            args = [a.sgxc, "-u", f"__p={k}"]
            for an, slot in layout.attributes(vs).items():
                args += ["-a", f"{an}={slot}"]
            r = subprocess.run(args + ["-o", os.path.join(pdir, "p"), os.path.join(pdir, "p.vert"),
                                       os.path.join(pdir, "p.frag")], capture_output=True, text=True)
            if r.returncode:
                print(f"{name}: probe k={k} failed: {r.stderr.strip()[-200:]}")
                break
            for P in (0, 1):
                bn = f"{name}__{P}_{k}"
                open(os.path.join(stage, "src", bn + ".vert"), "w").write(vs)
                open(os.path.join(stage, "src", bn + ".frag"), "w").write(fs)
                for st in (("frag",) if kind == "frag" else ("vert", "frag")):
                    shutil.copy(os.path.join(pdir, f"p.{st}.usp"), os.path.join(stage, "sub", f"{bn}.{st}.usp"))
                vals, _ = markers(lv, 1 + 100 * P)
                lines += block(bn, vs, fs, vals, lv, given.get(name))
            plan.append((name, k))
    open(os.path.join(stage, "list"), "w").write("\n".join(lines) + "\n")
    shutil.copy(os.path.join(hwcmp.BUILD, "glt"), stage)
    shutil.copy(os.path.join(hwcmp.BUILD, "uspsub.so"), stage)
    print(f"{len(plan) * 2} probe draws staged")
    tar = subprocess.run(["tar", "cf", "-", "-C", stage, "."], capture_output=True, check=True).stdout
    hwcmp.sh(f"rm -rf {hwcmp.BOX} && mkdir -p {hwcmp.BOX} && tar xf - -C {hwcmp.BOX}", inp=tar)
    r = hwcmp.sh(f"cd {hwcmp.BOX} && mkdir -p out && n=$(dmesg | wc -l); R=/opt/sgx-i386; "
                 f"LD_PRELOAD=\"$R/usr/lib/sgx-busidshim.so {hwcmp.BOX}/uspsub.so\" USPSUB_DIR={hwcmp.BOX}/sub "
                 f"$R/lib/ld-linux.so.2 --library-path $R/usr/lib:$R/lib {hwcmp.BOX}/glt list out > log 2>&1; "
                 f"echo rc=$?; dmesg | tail -n +$((n+1)) | grep -i -E 'recover|lockup' | head -3; "
                 f"tar cf res.tar out log", check=False)
    print(r.stdout.decode(errors="replace").strip())
    res = hwcmp.sh(f"cat {hwcmp.BOX}/res.tar").stdout
    rdir = os.path.join(a.outdir, "calib", "res")
    shutil.rmtree(rdir, ignore_errors=True)
    os.makedirs(rdir)
    subprocess.run(["tar", "xf", "-", "-C", rdir], input=res, check=True)

    # decode: constant k holds component j of leaf nm when pass 1 reads pass 0 + 100
    found = {}
    for name, k in plan:
        px = []
        for P in (0, 1):
            f = os.path.join(rdir, "out", f"{name}__{P}_{k}.rgba")
            d = open(f, "rb").read() if os.path.exists(f) else b""
            o = (hwcmp.H // 2 * hwcmp.W + hwcmp.W // 2) * 4
            px.append(d[o:o + 4])
        if len(px[0]) < 4 or len(px[1]) < 4:
            continue
        for j in range(4):
            c0, c1 = px[0][j], px[1][j]
            if c1 == c0 + 100 and c0 >= 1:
                found.setdefault(name, {})[k + j] = c0
    os.makedirs(os.path.join(a.outdir, "calib"), exist_ok=True)
    for name in sorted({n for n, _ in plan}):
        kind = "vert" if open(glob.glob(os.path.join(a.outdir, f"*-{name}.usp"))[0], "rb").read()[0x10] == 1 else "frag"
        src = open(os.path.join(a.outdir, "work", f"{name}.{kind}")).read()
        _, codes = markers(leaves(src), 1)
        inv = {c: (nm, j) for nm, cs in codes.items() for j, c in enumerate(cs)}
        out = {}
        for kk, c in sorted(found.get(name, {}).items()):
            if c in inv:
                nm, j = inv[c]
                out.setdefault(nm, [None] * len(codes[nm]))[j] = kk
        json.dump(out, open(os.path.join(a.outdir, "calib", f"{name}.json"), "w"), indent=1)
        print(f"{name:16} " + " ".join(f"{nm}={v}" for nm, v in out.items()))


if __name__ == "__main__":
    main()
