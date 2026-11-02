# A rough expansion of dEQP-GLES2 shader .test cases (usc work aid; set T to a dEQP build).
"""Expand dEQP-GLES2 shader .test cases roughly (inputs as uniforms, outputs into
gl_FragColor / gl_Position) and compile each variant with sgxc the way the
gallium driver does (-p -L -D -F). Prints error counts and examples."""
import os, re, subprocess, sys, collections
T = os.path.expanduser("~/.cache/sgx545/deqp/out/gles2/shaders")
S = os.path.expanduser("~/.cache/sgx545/usc/mesa262/build/src/sgx545/sgxc/sgxc")
W = os.path.expanduser("~/.cache/sgx545/usc/dq/w")
os.makedirs(W, exist_ok=True)
CASE = re.compile(r'^\s*case\s+(\S+)(.*?)^\s*end\s*$', re.S | re.M)
SRC = re.compile(r'^\s*(both|vertex|fragment)\s+""(.*?)""', re.S | re.M)
VAL = re.compile(r'^\s*(input|output|uniform)\s+(?:(lowp|mediump|highp)\s+)?(\w+)\s+(\w+)', re.M)

def conv(t, n):
    if t.startswith("mat"): return f"{n}[0][0]"
    if t[-1].isdigit() and t[0] in "vib": return f"float({n}.x)"
    return f"float({n})"

def expand(src, vals, stage):
    decl, outp = [], []
    for kind, prec, t, n in vals:
        p = (prec + " ") if prec else ""
        if kind == "output":
            decl.append(f"{p}{t} {n};")
            outp.append(conv(t, n))
        elif VARY and stage == "fragment" and t in ("float", "vec2", "vec3", "vec4"):
            decl.append(f"varying {p}{t} {n};")
        else:
            decl.append(f"uniform {p}{t} {n};")
    s = src.replace("${DECLARATIONS}", "\n".join(decl))
    sm = " + ".join(outp) if outp else "0.0"
    if stage == "fragment":
        s = s.replace("${OUTPUT}", f"gl_FragColor = vec4({sm});")
    else:
        s = s.replace("${DECLARATIONS}", "")
        if "dEQP_Position" not in s:
            s = s.replace("void main", "attribute highp vec4 dEQP_Position;\nvoid main", 1)
        s = s.replace("${OUTPUT}", f"gl_Position = dEQP_Position + vec4({sm});")
    s = s.replace("${POSITION_FRAG_COLOR}", "gl_FragColor").replace("${VERTEX_OUTPUT}", "gl_Position = dEQP_Position;")
    s = s.replace("${FRAGMENT_OUTPUT}", "gl_FragColor = vec4(1.0);")
    s = re.sub(r"\$\{[A-Z_]+\}", "", s)
    return s

VARY = os.environ.get("VARY") == "1"
VS = "attribute highp vec4 dEQP_Position;\nvoid main() { gl_Position = dEQP_Position; }\n"
FS = "precision mediump float;\nvoid main() { gl_FragColor = vec4(1.0); }\n"
errs = collections.Counter(); ex = {}; n = ok = 0
files = sys.argv[1:] or sorted(f[:-5] for f in os.listdir(T) if f.endswith(".test") and not f.startswith(("invalid", "reserved", "preproc", "keywords", "qualification")))
for f in files:
    txt = open(f"{T}/{f}.test").read()
    for m in CASE.finditer(txt):
        name, body = m.group(1), m.group(2)
        if re.search(r"expect\s+compile_fail|expect\s+link_fail", body):
            continue
        vals = VAL.findall(body)
        srcs = {k: v for k, v in SRC.findall(body)}
        variants = []
        if "both" in srcs:
            variants = [("fragment", VS, expand(srcs["both"], vals, "fragment")), ("vertex", expand(srcs["both"], vals, "vertex"), FS)]
        elif "vertex" in srcs or "fragment" in srcs:
            variants = [("pair", expand(srcs.get("vertex", VS), vals, "vertex"), expand(srcs.get("fragment", FS), vals, "fragment"))]
        for var, vs, fs in variants:
            n += 1
            base = f"{W}/{f}.{name}.{var}"
            open(base + ".vert", "w").write(vs); open(base + ".frag", "w").write(fs)
            r = subprocess.run([S, "-p", "-L", "-D", "-F", "-o", base, base + ".vert", base + ".frag"], capture_output=True, text=True)
            out = r.stdout + r.stderr
            e = [l for l in out.splitlines() if l.startswith("error:") or "Assertion" in l or "link failed" in l or "compile failed" in l and False]
            if r.returncode == 0:
                ok += 1; continue
            key = e[0] if e else (f"exit {r.returncode}: " + (out.strip().splitlines() or ["?"])[-1])
            key = re.sub(r"ssa \d+", "ssa N", re.sub(r"\(form \d+.*?\)", "(...)", key))[:150]
            errs[key] += 1
            ex.setdefault(key, []).append(f"{f}.{name}.{var}")
print(f"{ok}/{n} compiled")
for k, c in errs.most_common():
    print(f"{c:5}  {k}\n        e.g. {', '.join(ex[k][:4])}")
