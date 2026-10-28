"""Compile a GLSL corpus (tools/isa/corpus/c*.glsl format: "@frag name" /
"@vert name" headers) with sgxc.

    python3 -I tools/usc/run_corpus.py SGXC corpus.glsl OUTDIR [-k name] [--layout]

Each fragment shader is paired with a pass-through vertex shader that
writes every varying it reads (and each vertex shader with a fragment
shader that reads its varyings), so Mesa can link the pair. Writes
OUTDIR/<nnn>-<name>.usp (the stage under test, numbered like the DDK corpus)
and OUTDIR/<nnn>-<name>.code, and prints one line per shader. With --layout
the uniform constant numbers follow glslcompiler's (layout.py).
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import layout  # noqa: E402


def parse(path):
    out = []
    for m in re.finditer(r"^@(frag|vert) (\S+)\n(.*?)(?=^@|\Z)", open(path).read(), re.S | re.M):
        out.append((m.group(1), m.group(2), m.group(3)))
    return out


DECL = re.compile(r"\b(attribute|varying|uniform)\s+(?:(lowp|mediump|highp)\s+)?(\w+)\s+(\w+)(\s*\[\s*\d+\s*\])?\s*;")


def decls(src):
    return [(m.group(1), m.group(2), m.group(3), m.group(4), m.group(5) or "") for m in DECL.finditer(src)]


def vert_for(frag):
    """a vertex shader writing every varying the fragment shader declares"""
    lines = ["attribute vec4 p;", "attribute vec4 a1;"]
    body = ["gl_Position = p;"]
    sw = {"float": ".x", "vec2": ".xy", "vec3": ".xyz", "vec4": ""}
    for q, prec, ty, name, arr in decls(frag):
        if q != "varying":
            continue
        lines.append(f"varying {prec + ' ' if prec else ''}{ty} {name}{arr};")
        if arr:
            n = int(arr.strip()[1:-1])
            body += [f"{name}[{i}] = a1{sw[ty]};" for i in range(n)]
        else:
            body.append(f"{name} = a1{sw.get(ty, '')};")
    return "\n".join(lines) + "\nvoid main() { " + " ".join(body) + " }\n"


def frag_for(vert):
    """a fragment shader reading every varying the vertex shader declares"""
    lines = ["precision highp float;"]
    terms = []
    sw = {"float": "vec4({0})", "vec2": "vec4({0}, 0.0, 0.0)", "vec3": "vec4({0}, 0.0)", "vec4": "{0}"}
    for q, prec, ty, name, arr in decls(vert):
        if q != "varying":
            continue
        lines.append(f"varying {prec + ' ' if prec else ''}{ty} {name}{arr};")
        if arr:
            for i in range(int(arr.strip()[1:-1])):
                terms.append(sw.get(ty, "{0}").format(f"{name}[{i}]"))
        else:
            terms.append(sw.get(ty, "{0}").format(name))
    col = " + ".join(terms) if terms else "vec4(0.0)"
    return "\n".join(lines) + f"\nvoid main() {{ gl_FragColor = {col}; }}\n"


def calib_pins(path):
    """-u pins from a calib.py result: each vector's first component, each
    matrix column's (components the probe didn't see are extrapolated)"""
    pins = {}
    for nm, ks in json.load(open(path)).items():
        known = [(j, k) for j, k in enumerate(ks) if k is not None]
        if not known:
            continue
        cols = 4 if len(ks) == 16 else 0
        if cols:
            for c in range(4):
                kc = [(j, k) for j, k in known if j // 4 == c]
                if kc:
                    pins[f"{nm}[{c}]"] = kc[0][1] - kc[0][0] % 4
        else:
            pins[nm] = known[0][1] - known[0][0]
    return pins


F16 = False


def compile_one(sgxc, kind, name, src, outdir, idx, use_layout):
    work = os.path.join(outdir, "work")
    os.makedirs(work, exist_ok=True)
    base = os.path.join(work, name)
    vs = vert_for(src) if kind == "frag" else src
    fs = src if kind == "frag" else frag_for(src)
    open(base + ".vert", "w").write(vs)
    open(base + ".frag", "w").write(fs)
    args = [sgxc]
    cal = os.path.join(outdir, "calib", name + ".json")
    if os.path.exists(cal):
        for u, k in calib_pins(cal).items():
            args += ["-u", f"{u}={k}"]
    elif use_layout:
        for u, k in layout.uniforms(src).items():
            args += ["-u", f"{u}={k}"]
        if kind == "vert":
            for a, k in layout.attributes(src).items():
                args += ["-a", f"{a}={k}"]
    # F16 for the mediump/lowp variants (their ints would become 16-bit too,
    # which the int-to-float lowering doesn't take in loops)
    if F16 and kind == "frag" and re.search(r"_[ml]$", name):
        args.append("-p")
    args += ["-o", base, base + ".vert", base + ".frag"]
    r = subprocess.run(args, capture_output=True, text=True)
    stem = os.path.join(outdir, f"{idx:03d}-{name}")
    ok = r.returncode == 0 and os.path.exists(f"{base}.{kind}.usp")
    if ok:
        for ext in ("usp", "code"):
            os.replace(f"{base}.{kind}.{ext}", f"{stem}.{ext}")
    line = [x for x in r.stdout.splitlines() if x.startswith(kind + ":")]
    err = (r.stderr.strip().splitlines() or [""])[-1] if not ok else ""
    log = ""
    if os.path.exists(f"{base}.{kind}.txt"):
        log = open(f"{base}.{kind}.txt").read()
    errs = [x for x in log.splitlines() if x.startswith("error:")]
    return ok, (line[0] if line else ""), (errs[0] if errs else err)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sgxc")
    ap.add_argument("corpus")
    ap.add_argument("outdir")
    ap.add_argument("-k")
    ap.add_argument("--layout", action="store_true")
    ap.add_argument("--f16", action="store_true", help="sgxc -p: mediump in F16")
    a = ap.parse_args()
    global F16
    F16 = a.f16
    os.makedirs(a.outdir, exist_ok=True)
    n_ok = n = 0
    for idx, (kind, name, src) in enumerate(parse(a.corpus)):
        if a.k and a.k not in name:
            continue
        n += 1
        ok, line, err = compile_one(a.sgxc, kind, name, src, a.outdir, idx, a.layout)
        n_ok += ok
        print(f"{'ok  ' if ok else 'FAIL'} {idx:03d} {name:16} {line if ok else err}")
    print(f"{n_ok}/{n} compiled")


if __name__ == "__main__":
    main()
