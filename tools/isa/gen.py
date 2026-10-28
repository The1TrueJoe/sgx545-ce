"""Generate a shader corpus for shc: one operation per shader, inputs from uniforms
so nothing constant-folds. Prints the corpus to stdout."""

FRAG = "precision {p} float;\nuniform vec4 u; uniform vec4 v; uniform vec4 w;\n{decl}void main() {{ {body} }}\n"

UNARY = {
    "neg": "-u", "abs": "abs(u)", "floor": "floor(u)", "fract": "fract(u)", "ceil": "ceil(u)",
    "sqrt": "sqrt(u)", "rsq": "inversesqrt(u)", "exp2": "exp2(u)", "log2": "log2(u)",
    "exp": "exp(u)", "log": "log(u)", "sin": "sin(u)", "cos": "cos(u)", "sign": "sign(u)",
    "rcp": "1.0 / u", "sat": "clamp(u, 0.0, 1.0)", "norm3": "vec4(normalize(u.xyz), 1.0)",
    "len3": "vec4(length(u.xyz))",
}
BINARY = {
    "add": "u + v", "sub": "u - v", "mul": "u * v", "div": "u / v", "min": "min(u, v)",
    "max": "max(u, v)", "step": "step(u, v)", "dp3": "vec4(dot(u.xyz, v.xyz))",
    "dp4": "vec4(dot(u, v))", "pow": "pow(u, v)", "mod": "mod(u, v)", "mad": "u * v + w",
    "mix": "mix(u, v, w)", "lt": "vec4(lessThan(u, v))", "dpx": "vec4(dot(u.xy, v.xy))",
}
SWIZ = ["yzwx", "xxxx", "wzyx", "zxyw", "yyyy", "wwww", "xyxy"]
CONST = {"c0": "vec4(0.0)", "c1": "vec4(1.0)", "c_half": "vec4(0.5)", "c_2": "vec4(2.0)",
         "c_mix": "vec4(0.0, 0.5, 1.0, 2.0)", "c_odd": "vec4(3.25, -7.5, 100.0, 0.125)"}
WRITE = {"wm_x": "vec4(u.x, 0.0, 0.0, 1.0)", "wm_xy": "vec4(u.xy, 0.0, 1.0)",
         "wm_w": "vec4(0.0, 0.0, 0.0, u.w)", "wm_uv": "vec4(u.x, v.y, u.z, v.w)"}


def frag(name, body, p="highp", decl=""):
    return f"@frag {name}\n" + FRAG.format(p=p, decl=decl, body=body)


def main():
    out = []
    for n, e in UNARY.items():
        out.append(frag(n, f"gl_FragColor = {e};"))
    for n, e in BINARY.items():
        out.append(frag(n, f"gl_FragColor = {e};"))
        out.append(frag(n + "_m", f"gl_FragColor = {e};", p="mediump"))
        out.append(frag(n + "_l", f"gl_FragColor = {e};", p="lowp"))
    for s in SWIZ:
        out.append(frag("swz_" + s, f"gl_FragColor = u.{s};"))
        out.append(frag("swzadd_" + s, f"gl_FragColor = u.{s} + v;"))
    for n, e in {**CONST, **WRITE}.items():
        out.append(frag(n, f"gl_FragColor = {e};"))
    out.append(frag("u_only", "gl_FragColor = u;"))
    out.append(frag("v_only", "gl_FragColor = v;"))
    out.append(frag("w_only", "gl_FragColor = w;"))
    vary = "varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;\n"
    out.append(frag("vary", "gl_FragColor = t;", decl=vary))
    out.append(frag("vary_mul", "gl_FragColor = t * u;", decl=vary))
    out.append(frag("tex", "gl_FragColor = texture2D(s, uv);", decl=vary))
    out.append(frag("tex_dep", "gl_FragColor = texture2D(s, u.xy);", decl=vary))
    out.append(frag("tex_bias", "gl_FragColor = texture2D(s, uv, 1.0);", decl=vary))
    out.append(frag("tex_proj", "gl_FragColor = texture2DProj(s, t);", decl=vary))
    out.append(frag("tex_cube", "gl_FragColor = textureCube(sc, t.xyz);", decl=vary))
    out.append(frag("tex_mul", "gl_FragColor = texture2D(s, uv) * u;", decl=vary))
    out.append(frag("discard", "if (u.x > 0.5) discard; gl_FragColor = v;"))
    out.append(frag("branch", "if (u.x > 0.0) gl_FragColor = v; else gl_FragColor = w;"))
    out.append(frag("loop4", "vec4 a = vec4(0.0); for (int i = 0; i < 4; i++) a += u * float(i); gl_FragColor = a;"))
    out.append(frag("loop_dyn", "vec4 a = vec4(0.0); for (int i = 0; i < 16; i++) { if (float(i) > u.x) break; a += v; } gl_FragColor = a;"))
    out.append("@vert vpos\nattribute vec4 p;\nvoid main() { gl_Position = p; }\n")
    out.append("@vert vmvp\nattribute vec4 p; uniform mat4 m;\nvoid main() { gl_Position = m * p; }\n")
    out.append("@vert vvary\nattribute vec4 p; attribute vec4 c; varying vec4 t;\nvoid main() { gl_Position = p; t = c; }\n")
    out.append("@vert varr\nattribute vec4 p; uniform vec4 a[8]; uniform int i;\nvoid main() { gl_Position = p + a[i]; }\n")
    out.append("@vert vtex\nattribute vec4 p; uniform sampler2D s;\nvoid main() { gl_Position = p + texture2DLod(s, p.xy, 0.0); }\n")
    print("".join(out), end="")


if __name__ == "__main__":
    main()
