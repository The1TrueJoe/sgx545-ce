"""Second shader corpus for shc: operations on varyings, so the compiler can't
hoist them into the secondary update program the way it does with uniform-only
math (gen.py's corpus). Families vary one thing at a time: opcode, precision,
source register, source bank, swizzle, write mask, modifier, constant, branch.
Prints the corpus to stdout."""

HEAD = "precision {p} float;\nvarying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;\n" \
       "varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;\nuniform vec4 u; uniform vec4 v;\n"

UNARY = {
    "neg": "-a * b", "abs": "abs(a) * b", "floor": "floor(a)", "fract": "fract(a)", "ceil": "ceil(a)",
    "sqrt": "sqrt(a)", "rsq": "inversesqrt(a)", "exp2": "exp2(a)", "log2": "log2(a)",
    "rcp": "1.0 / a", "sin": "sin(a)", "cos": "cos(a)", "sign": "sign(a)", "sat": "clamp(a, 0.0, 1.0)",
    "norm3": "vec4(normalize(a.xyz), 1.0)", "mov": "a",
}
BINARY = {
    "add": "a + b", "sub": "a - b", "mul": "a * b", "div": "a / b", "min": "min(a, b)",
    "max": "max(a, b)", "step": "step(a, b)", "dp3": "vec4(dot(a.xyz, b.xyz))", "dp4": "vec4(dot(a, b))",
    "mad": "a * b + c", "mix": "mix(a, b, c)", "lt": "vec4(lessThan(a, b))", "pow": "pow(a, b)",
    "mod": "mod(a, b)", "satmul": "clamp(a * b, 0.0, 1.0)",
}
# scalar (one-component) forms: highp ops on .x only
SCALAR = {
    "s_add": "a.x + b.x", "s_mul": "a.x * b.x", "s_mad": "a.x * b.x + c.x", "s_rcp": "1.0 / a.x",
    "s_rsq": "inversesqrt(a.x)", "s_exp2": "exp2(a.x)", "s_log2": "log2(a.x)", "s_min": "min(a.x, b.x)",
    "s_max": "max(a.x, b.x)", "s_sub": "a.x - b.x", "s_frc": "fract(a.x)", "s_dp3": "dot(a.xyz, b.xyz)",
    "s_mul_yz": "a.y * b.z", "s_mul_wx": "a.w * b.x", "s_neg": "-a.x * b.x", "s_abs": "abs(a.x) * b.x",
    "s_mulc": "a.x * 3.0", "s_addc": "a.x + 0.5", "s_mulu": "a.x * u.x", "s_mulu_y": "a.x * u.y",
    "s_mulv": "a.x * v.z",
}
REGS = "abcdefgh"
SWIZ = ["xyzw", "yzwx", "xxxx", "wzyx", "zxyw", "yyyy", "wwww", "xyxy", "zzzz", "xwyz"]


def frag(name, body, p="highp"):
    return f"@frag {name}\n" + HEAD.format(p=p) + f"void main() {{ {body} }}\n"


def main():
    out = []
    for n, e in {**UNARY, **BINARY}.items():
        for p, suf in (("highp", ""), ("mediump", "_m"), ("lowp", "_l")):
            out.append(frag(n + suf, f"gl_FragColor = {e};", p))
    for n, e in SCALAR.items():
        out.append(frag(n, f"gl_FragColor = vec4({e});"))
        out.append(frag(n + "_m", f"gl_FragColor = vec4({e});", "mediump"))
    # source register numbers: one product per varying pair
    for i in range(8):
        out.append(frag(f"r_a{REGS[i]}", f"gl_FragColor = vec4(a.x * {REGS[i]}.x);"))
        out.append(frag(f"r_{REGS[i]}a", f"gl_FragColor = vec4({REGS[i]}.x * a.y);"))
        out.append(frag(f"rv_a{REGS[i]}", f"gl_FragColor = a * {REGS[i]};"))
    # swizzles on a vec4 mul
    for s in SWIZ:
        out.append(frag("sw_" + s, f"gl_FragColor = a.{s} * b;"))
        out.append(frag("sw2_" + s, f"gl_FragColor = a * b.{s};"))
        out.append(frag("swm_" + s, f"gl_FragColor = a.{s} * b;", "mediump"))
    # write masks: the result lands in some components only
    for m in ["x", "y", "z", "w", "xy", "xz", "yw", "xyz", "yzw"]:
        out.append(frag("wm_" + m, f"vec4 r = b; r.{m} = (a * c).{m}; gl_FragColor = r;"))
        out.append(frag("wmm_" + m, f"vec4 r = b; r.{m} = (a * c).{m}; gl_FragColor = r;", "mediump"))
    # constants
    for n, k in {"k0": "0.0", "k1": "1.0", "k2": "2.0", "kh": "0.5", "kn1": "-1.0", "k3": "3.0",
                 "k100": "100.0", "kodd": "3.25", "kpi": "3.14159"}.items():
        out.append(frag("kmul_" + n, f"gl_FragColor = a * {k};"))
        out.append(frag("kadd_" + n, f"gl_FragColor = a + {k};"))
        out.append(frag("kmulm_" + n, f"gl_FragColor = a * {k};", "mediump"))
    out.append(frag("kvec", "gl_FragColor = a * vec4(1.0, 2.0, 3.0, 4.0);"))
    # uniforms against varyings: bank differences
    out.append(frag("u_mul", "gl_FragColor = a * u;"))
    out.append(frag("u_add", "gl_FragColor = a + u;"))
    out.append(frag("v_mul", "gl_FragColor = a * v;"))
    out.append(frag("uv_mad", "gl_FragColor = a * u + v;"))
    out.append(frag("u_mul_m", "gl_FragColor = a * u;", "mediump"))
    # control flow
    out.append(frag("if_gt", "if (a.x > b.x) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("if_lt", "if (a.x < b.x) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("if_eq", "if (a.x == b.x) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("if_ge", "if (a.x >= b.x) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("if_ne", "if (a.x != b.x) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("if_z", "if (a.x > 0.0) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("if_mul", "vec4 r = c; if (a.x > b.x) r = c * d; gl_FragColor = r;"))
    out.append(frag("discard", "if (a.x > b.x) discard; gl_FragColor = c;"))
    out.append(frag("loop_v", "vec4 r = vec4(0.0); for (int i = 0; i < 8; i++) { if (r.x > a.x) break; r += b; } gl_FragColor = r;"))
    # texture reads in the main program
    tex = "uniform sampler2D s; uniform sampler2D s2;\n"
    out.append(f"@frag t_dep\n" + HEAD.format(p="highp") + tex + "void main() { gl_FragColor = texture2D(s, a.xy * b.xy); }\n")
    out.append(f"@frag t_two\n" + HEAD.format(p="highp") + tex + "void main() { gl_FragColor = texture2D(s, a.xy) + texture2D(s2, b.xy); }\n")
    out.append(f"@frag t_lod\n" + HEAD.format(p="highp") + tex + "void main() { gl_FragColor = texture2D(s, a.xy * b.xy, 2.0); }\n")
    out.append(f"@frag t_mul\n" + HEAD.format(p="mediump") + tex + "void main() { gl_FragColor = texture2D(s, a.xy) * b; }\n")
    # vertex: attributes, matrices, varyings out
    vh = "attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;\n"
    for n, body in {"v_mvp": "gl_Position = m * p;", "v_add": "gl_Position = p + q;",
                    "v_mul": "gl_Position = p * q;", "v_mad": "gl_Position = p * q + n;",
                    "v_dp4": "gl_Position = vec4(dot(p, q));", "v_rsq": "gl_Position = inversesqrt(p);",
                    "v_vary": "gl_Position = p; o = q * k;", "v_k": "gl_Position = p * k;",
                    "v_norm": "gl_Position = vec4(normalize(p.xyz), 1.0);",
                    "v_psize": "gl_Position = p; gl_PointSize = q.x;"}.items():
        out.append(f"@vert {n}\n" + vh + f"void main() {{ {body} }}\n")
    print("".join(out), end="")


if __name__ == "__main__":
    main()
