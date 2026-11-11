"""Integer and conversion ops of GLSL 1.30 vertex shaders, compiled with sgxc
and run through usse_sim.py on random inputs, against exact results.

    python3 -I tools/usc/int_check.py SGXC SGX_DISASM OUTDIR [-n 300]

Each op is `r0 = expr` over ivec4/uvec4 attributes a (pa0-3) and b (pa4-7),
written to a flat varying (o4-7). A failure is a compiler bug under
docs/ISA.md's semantics as usse_sim.py models them; the hwt suite
(tools/isa/hwt_int.py) checks those semantics on the hardware.
"""
import argparse, os, random, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usse_sim as U
ap = argparse.ArgumentParser()
ap.add_argument("sgxc"); ap.add_argument("disasm"); ap.add_argument("out")
ap.add_argument("-n", type=int, default=300)
ap.add_argument("--seed", type=int, default=1)
args = ap.parse_args()
random.seed(args.seed)
SGXC, DIS, W = args.sgxc, args.disasm, args.out
os.makedirs(W, exist_ok=True)
M = 0xffffffff
def s(v): v &= M; return v - (1 << 32) if v >> 31 else v
def tdiv(a, b): q = abs(a) // abs(b); return q if (a < 0) == (b < 0) else -q
OPS = {
 "add": ("ivec4", "a + b", lambda a, b: a + b),
 "sub": ("ivec4", "a - b", lambda a, b: a - b),
 "neg": ("ivec4", "-a", lambda a, b: -a),
 "mul": ("ivec4", "a * b", lambda a, b: s(a) * s(b)),
 "umul": ("uvec4", "a * b", lambda a, b: a * b),
 "and": ("ivec4", "a & b", lambda a, b: a & b),
 "or": ("ivec4", "a | b", lambda a, b: a | b),
 "xor": ("ivec4", "a ^ b", lambda a, b: a ^ b),
 "not": ("ivec4", "~a", lambda a, b: ~a),
 "shl": ("ivec4", "a << (b & 31)", lambda a, b: a << (b & 31)),
 "asr": ("ivec4", "a >> (b & 31)", lambda a, b: s(a) >> (b & 31)),
 "shr": ("uvec4", "a >> (b & 31u)", lambda a, b: a >> (b & 31)),
 "shl5": ("ivec4", "a << 5", lambda a, b: a << 5),
 "and_k": ("ivec4", "a & 0x0ff00000", lambda a, b: a & 0x0ff00000),
 "lt": ("ivec4", "ivec4(lessThan(a, b))", lambda a, b: int(s(a) < s(b))),
 "ge": ("ivec4", "ivec4(greaterThanEqual(a, b))", lambda a, b: int(s(a) >= s(b))),
 "ult": ("uvec4", "uvec4(lessThan(a, b))", lambda a, b: int(a < b)),
 "uge": ("uvec4", "uvec4(greaterThanEqual(a, b))", lambda a, b: int(a >= b)),
 "eq": ("ivec4", "ivec4(equal(a, b))", lambda a, b: int(a == b)),
 "ne": ("ivec4", "ivec4(notEqual(a, b))", lambda a, b: int(a != b)),
 "lt0": ("ivec4", "ivec4(lessThan(a, ivec4(0)))", lambda a, b: int(s(a) < 0)),
 "gt0": ("ivec4", "ivec4(greaterThan(a, ivec4(0)))", lambda a, b: int(s(a) > 0)),
 "min": ("ivec4", "min(a, b)", lambda a, b: min(s(a), s(b))),
 "max": ("ivec4", "max(a, b)", lambda a, b: max(s(a), s(b))),
 "umin": ("uvec4", "min(a, b)", lambda a, b: min(a, b)),
 "umax": ("uvec4", "max(a, b)", lambda a, b: max(a, b)),
 "abs": ("ivec4", "abs(a)", lambda a, b: abs(s(a))),
 "sign": ("ivec4", "sign(a)", lambda a, b: (s(a) > 0) - (s(a) < 0)),
 "sel": ("ivec4", "ivec4(a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y, a.z < b.z ? b.z : a.z, a.w < b.w ? b.w : a.w)", lambda a, b: b if s(a) < s(b) else a),
 "div": ("ivec4", "a / b", lambda a, b: tdiv(s(a), s(b))),
 "mod": ("ivec4", "a % b", lambda a, b: s(a) - tdiv(s(a), s(b)) * s(b)),
 "udiv": ("uvec4", "a / b", lambda a, b: a // b),
 "umod": ("uvec4", "a % b", lambda a, b: a % b),
 "div7": ("ivec4", "a / 7", lambda a, b: tdiv(s(a), 7)),
 "udiv7": ("uvec4", "a / 7u", lambda a, b: a // 7),
 "i2f": ("ivec4>vec4", "vec4(a)", lambda a, b: U.b(float(s(a)))),
 "u2f": ("uvec4>vec4", "vec4(a)", lambda a, b: U.b(float(a))),
 "f2i": ("vec4>ivec4", "ivec4(a)", lambda a, b: int(U.f(a))),
 "f2u": ("vec4>uvec4", "uvec4(a)", lambda a, b: int(U.f(a))),
}
def rnd(kind):
    r = random.random()
    if r < 0.25: return random.choice([0, 1, 2, 3, 7, M, M - 1, 0x7fffffff, 0x80000000, 0x80000001, 0xffff, 0x10000])
    if r < 0.5: return random.randint(0, 200) if random.random() < .5 else (-random.randint(1, 200)) & M
    return random.getrandbits(32)
bad = 0
for name, (ty, expr, fn) in OPS.items():
    ver = 330 if "Bits" in expr else 130
    ty, _, oty = ty.partition(">"); oty = oty or ty
    vs = f"#version {ver}\nin {ty} a; in {ty} b;\nflat out {oty} r0;\nvoid main() {{ gl_Position = vec4(0.0); r0 = {expr}; }}\n"
    fs = f"#version {ver}\nflat in {oty} r0;\nout vec4 col;\nvoid main() {{ col = vec4(r0); }}\n"
    open(f"{W}/{name}.vert", "w").write(vs); open(f"{W}/{name}.frag", "w").write(fs)
    p = subprocess.run([SGXC, "-G", str(ver), "-o", f"{W}/{name}", f"{W}/{name}.vert", f"{W}/{name}.frag"], capture_output=True, text=True)
    if p.returncode:
        print(name, "COMPILE FAIL", p.stderr.strip().splitlines()[-3:]); bad += 1; continue
    lines = U.disasm(f"{W}/{name}.vert.code", DIS)
    n = len(lines); errs = 0
    for t in range(args.n):
        av = [rnd(ty) for _ in range(4)]; bv = [rnd(ty) for _ in range(4)]
        if ty == "vec4":
            lim = 2**31 if name == "f2i" else 2**32
            av = [U.b(random.choice([random.uniform(-lim, lim) if name == "f2i" else random.uniform(0, lim), random.uniform(-300, 300) if name == "f2i" else random.uniform(0, 300), float(random.randint(-70000, 70000) if name == "f2i" else random.randint(0, 140000)), 0.5, 65535.5, 65536.0, 65537.0])) for _ in range(4)]
            av = [x if abs(U.f(x)) < lim else U.b(1.0) for x in av]
        if name == "mod":
            av = [x & 0x7fffffff for x in av]; bv = [x & 0x7fffffff for x in bv]
        if name in ("div", "mod", "udiv", "umod"):
            bv = [x if x else 1 for x in bv]
            if name in ("div", "mod"): bv = [x if not (x == M) else 3 for x in bv]
        regs = {("pa", k): av[k] for k in range(4)}; regs.update({("pa", 4 + k): bv[k] for k in range(4)})
        sim = U.Sim(regs)
        try:
            sim.run(lines)
        except Exception as e:
            print(name, "SIM", repr(e)); errs += 1; break
        for k in range(4):
            want = fn(av[k], bv[k]) & M
            got = sim.r.get(("o", 4 + k))
            if got != want:
                if errs < 3: print(f"{name}: a={av[k]:#x} b={bv[k]:#x} want {want:#x} got {got if got is None else hex(got)}")
                errs += 1
    print(f"{name:6} {n:4} instr  {'ok' if not errs else f'{errs} wrong'}"); bad += bool(errs)
sys.exit(bad)
