"""The shaders gl4es generates for Minecraft 1.7.10 (mc.glsl, from fpegen.c:
gl4es's own fpe_VertexShader/fpe_FragmentShader + ConvertShader for the
game's fixed-function states), compiled with sgxc and drawn on the EA3
against the DDK.

    python3 -I usc/tests/gl4es/mc_run.py SGXC OUT [--calibrate] [--draw]

OUT/f: one entry per program with the fragment shader under test, OUT/v
with the vertex shader under test (both stages are compiled; hwcmp.py
--stage both substitutes both). --calibrate runs usc/hw/calib.py on each to
learn glslcompiler's constant numbers, and the compile uses them.
The draws use game-like state: a cut-out atlas texture (alpha test
discards), a lightmap with its texture matrix, per-vertex colour, and
linear fog over a tilted quad.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
USC = os.path.join(HERE, "..", "..")
sys.path.insert(0, USC)
sys.path.insert(0, os.path.join(USC, "hw"))
import layout  # noqa: E402
import run_corpus  # noqa: E402

COLOR = [1, 0.9, 0.8, 1, 0.5, 0.6, 1, 1, 0.8, 1, 0.5, 0.9, 0.3, 0.4, 0.6, 1]
TC0 = [0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1]
TC1 = [16, 16, 0, 1, 240, 16, 0, 1, 16, 240, 0, 1, 240, 240, 0, 1]
MV = [1, 0, 0, 0, 0, 0.8, 0.9, 0, 0, 0, 1, 0, 0, 0, -2.5, 1]
MVP = [0.95, 0, 0, 0, 0, 0.95, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 1]
TM1 = [1 / 256, 0, 0, 0, 0, 1 / 256, 0, 0, 0, 0, 1, 0, 1 / 32, 1 / 32, 0, 1]
FOG = {"color": [0.6, 0.7, 0.9, 1.0], "density": [1.0], "start": [1.0], "end": [4.0], "scale": [1 / 3]}


def fmt(v):
    return " ".join(f"{x:.9g}" for x in v)


def block(name, vs, fs):
    src = vs + fs
    lines = [f"test {name}", "pos _gl4es_Vertex", f"vs @BOX@/src/{name}.vert", f"fs @BOX@/src/{name}.frag"]
    loc = 1
    for a, v in (("_gl4es_Color", COLOR), ("_gl4es_MultiTexCoord0", TC0), ("_gl4es_MultiTexCoord1", TC1)):
        if re.search(r"attribute\s+\w+\s+vec4\s+" + a + ";", vs):
            lines.append(f"attr {a} {loc} {fmt(v)}")
            loc += 1
    for u, v in (("_gl4es_ModelViewMatrix", MV), ("_gl4es_ModelViewProjectionMatrix", MVP),
                 ("_gl4es_TextureMatrix_1", TM1)):
        if u in src:
            lines.append(f"m {u} {fmt(v)}")
    if "_gl4es_Fog" in src:
        for k, v in FOG.items():
            lines.append(f"u _gl4es_Fog.{k} {fmt(v)}")
    if "_gl4es_AlphaRef" in src:
        lines.append("u _gl4es_AlphaRef 25.5")       # alpha > 0.1, as gl4es passes it (ref * 255)
    for i in range(2):
        if f"_gl4es_TexSampler_{i}" in src:
            lines.append(f"i _gl4es_TexSampler_{i} {i}")
            lines.append(f"tex {i} {'cut' if i == 0 else 'grad'}")
    lines.append("end")
    return lines


def pins(d, name, stage):
    p = os.path.join(d, "calib", name + ".json")
    if not os.path.exists(p):
        return []
    return [f"{stage}:{u}={k}" for u, k in run_corpus.calib_pins(p).items()]


def build(sgxc, out, progs, which, f16=False):
    d = os.path.join(out, which)
    work = os.path.join(d, "work")
    os.makedirs(work, exist_ok=True)
    lines = []
    for i, (name, vs, fs) in enumerate(progs):
        base = os.path.join(work, name)
        open(base + ".vert", "w").write(vs)
        open(base + ".frag", "w").write(fs)
        args = [sgxc]
        for a, k in layout.attributes(vs).items():
            args += ["-a", f"{a}={k}"]
        for p in pins(os.path.join(out, "f"), name, "frag") + pins(os.path.join(out, "v"), name, "vert"):
            args += ["-u", p]
        kind = "frag" if which == "f" else "vert"
        # mediump fragment shaders in F16, as the DDK computes them; the vertex
        # shader (the other stage here) in F32
        if f16 and kind == "frag":
            rv = subprocess.run(args + ["-o", base, base + ".vert", base + ".frag"], capture_output=True, text=True)
            args = args + ["-p"]
            if rv.returncode == 0:
                os.replace(base + ".vert.usp", base + ".vert.usp.f32")
        r = subprocess.run(args + ["-o", base, base + ".vert", base + ".frag"], capture_output=True, text=True)
        if f16 and kind == "frag" and os.path.exists(base + ".vert.usp.f32"):
            os.replace(base + ".vert.usp.f32", base + ".vert.usp")
        ok = r.returncode == 0
        if ok:
            os.replace(f"{base}.{kind}.usp", os.path.join(d, f"{i:03d}-{name}.usp"))
            os.replace(f"{base}.{kind}.code", os.path.join(d, f"{i:03d}-{name}.code"))
        print(f"{which} {name:12} {'ok  ' if ok else 'FAIL'} {r.stdout.strip()} {r.stderr.strip()[-300:] if not ok else ''}")
        lines += block(name, vs, fs)
    open(os.path.join(d, "tests.list"), "w").write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sgxc")
    ap.add_argument("out")
    ap.add_argument("--calibrate", action="store_true")
    ap.add_argument("--draw", action="store_true")
    ap.add_argument("--f16", action="store_true", help="fragment shaders in F16 (sgxc -p)")
    a = ap.parse_args()
    src = open(os.path.join(HERE, "mc.glsl")).read()
    sh = {}
    for m in re.finditer(r"^@(vert|frag) (\S+)\n(.*?)(?=^@|\Z)", src, re.S | re.M):
        sh.setdefault(m.group(2), {})[m.group(1)] = m.group(3)
    progs = [(n, s["vert"], s["frag"]) for n, s in sh.items()]
    for w in ("f", "v"):
        build(a.sgxc, a.out, progs, w, a.f16)
    if a.calibrate:
        for w in ("f", "v"):
            subprocess.run([sys.executable, "-I", os.path.join(USC, "hw", "calib.py"), a.sgxc,
                            os.path.join(a.out, w)], check=True)
        for w in ("f", "v"):
            build(a.sgxc, a.out, progs, w, a.f16)
    if a.draw:
        for w in ("f", "v"):
            for st in ("one", "both"):
                print(f"== {w} under test, {st} stage(s) substituted")
                subprocess.run([sys.executable, "-I", os.path.join(USC, "hw", "hwcmp.py"), os.path.join(a.out, w)]
                               + (["--stage", "both"] if st == "both" else []), check=False)


if __name__ == "__main__":
    main()
