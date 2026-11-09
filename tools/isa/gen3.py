"""Third shader corpus for shc: texture sampling, flow control, integer and
bool code, indexing and vertex outputs. Each family varies one thing from a
base shader so a diff of two disassemblies names the bits that thing moves.
Prints the corpus to stdout."""

VARY = "varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;\n"
UNIF = "uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;\n"
SAMP = "uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;\n" \
       "uniform samplerCube sc;\n"


def frag(name, body, p="highp", decl=""):
    return f"@frag {name}\nprecision {p} float;\n" + VARY + UNIF + SAMP + decl + f"void main() {{ {body} }}\n"


VHEAD = "attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;\n" \
        "uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;\n" \
        "uniform sampler2D s;\n" \
        "varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;\n"


def vert(name, body, decl=""):
    return f"@vert {name}\n" + VHEAD + decl + f"void main() {{ {body} }}\n"


def texture():
    out = []
    # non-dependent reads (USP sample blocks), one variable at a time
    out.append(frag("tx_nd", "gl_FragColor = texture2D(s, a.xy);"))
    out.append(frag("tx_nd_s1", "gl_FragColor = texture2D(s1, a.xy);"))
    out.append(frag("tx_nd_s3", "gl_FragColor = texture2D(s3, a.xy);"))
    out.append(frag("tx_nd_b", "gl_FragColor = texture2D(s, b.xy);"))
    out.append(frag("tx_nd_zw", "gl_FragColor = texture2D(s, a.zw);"))
    out.append(frag("tx_nd_m", "gl_FragColor = texture2D(s, a.xy);", "mediump"))
    out.append(frag("tx_nd_l", "gl_FragColor = texture2D(s, a.xy);", "lowp"))
    out.append(frag("tx_nd_proj3", "gl_FragColor = texture2DProj(s, a.xyz);"))
    out.append(frag("tx_nd_proj4", "gl_FragColor = texture2DProj(s, a);"))
    out.append(frag("tx_nd_cube", "gl_FragColor = textureCube(sc, a.xyz);"))
    out.append(frag("tx_nd_two", "gl_FragColor = texture2D(s, a.xy) * texture2D(s1, b.xy);"))
    out.append(frag("tx_nd_same2", "gl_FragColor = texture2D(s, a.xy) * texture2D(s, b.xy);"))
    # dependent reads: computed coordinates
    dep = "a.xy * b.xy"
    out.append(frag("tx_dep", f"gl_FragColor = texture2D(s, {dep});"))
    out.append(frag("tx_dep_s1", f"gl_FragColor = texture2D(s1, {dep});"))
    out.append(frag("tx_dep_s2", f"gl_FragColor = texture2D(s2, {dep});"))
    out.append(frag("tx_dep_s3", f"gl_FragColor = texture2D(s3, {dep});"))
    out.append(frag("tx_dep_m", f"gl_FragColor = texture2D(s, {dep});", "mediump"))
    out.append(frag("tx_dep_l", f"gl_FragColor = texture2D(s, {dep});", "lowp"))
    out.append(frag("tx_dep_bias", f"gl_FragColor = texture2D(s, {dep}, 1.5);"))
    out.append(frag("tx_dep_biasv", f"gl_FragColor = texture2D(s, {dep}, c.x);"))
    out.append(frag("tx_nd_bias", "gl_FragColor = texture2D(s, a.xy, 1.5);"))
    out.append(frag("tx_dep_proj3", "gl_FragColor = texture2DProj(s, a.xyz * b.xyz);"))
    out.append(frag("tx_dep_proj4", "gl_FragColor = texture2DProj(s, a * b);"))
    out.append(frag("tx_dep_projb", "gl_FragColor = texture2DProj(s, a.xyz * b.xyz, 1.0);"))
    out.append(frag("tx_dep_cube", "gl_FragColor = textureCube(sc, a.xyz * b.xyz);"))
    out.append(frag("tx_dep_cubeb", "gl_FragColor = textureCube(sc, a.xyz * b.xyz, 2.0);"))
    out.append(frag("tx_dep_u", "gl_FragColor = texture2D(s, u.xy);"))
    out.append(frag("tx_dep_uv", "gl_FragColor = texture2D(s, a.xy + u.xy);"))
    out.append(frag("tx_dep_chain", "gl_FragColor = texture2D(s1, texture2D(s, a.xy).xy);"))
    out.append(frag("tx_dep_x", "gl_FragColor = vec4(texture2D(s, a.xy * b.xy).x);"))
    out.append(frag("tx_dep_w", "gl_FragColor = vec4(texture2D(s, a.xy * b.xy).w);"))
    out.append(frag("tx_dep_add", "gl_FragColor = texture2D(s, a.xy * b.xy) + c;"))
    out.append(frag("tx_dep_add_m", "gl_FragColor = texture2D(s, a.xy * b.xy) + c;", "mediump"))
    out.append(frag("tx_dep_add_l", "gl_FragColor = texture2D(s, a.xy * b.xy) + c;", "lowp"))
    out.append(frag("tx_dep_two", "gl_FragColor = texture2D(s, a.xy * b.xy) * texture2D(s1, c.xy * d.xy);"))
    out.append(frag("tx_dep_if", "vec4 r = c; if (a.x > b.x) r = texture2D(s, a.xy * b.xy); gl_FragColor = r;"))
    out.append(frag("tx_dep_loop", "vec4 r = vec4(0.0); for (int i = 0; i < 4; i++) "
                    "r += texture2D(s, a.xy * float(i)); gl_FragColor = r;"))
    # vertex texture fetch
    out.append(vert("vtx_lod", "gl_Position = p + texture2DLod(s, q.xy, 0.0);"))
    out.append(vert("vtx_lod2", "gl_Position = p + texture2DLod(s, q.xy, r.x);"))
    out.append(vert("vtx_tex", "gl_Position = p + texture2D(s, q.xy);"))
    out.append(vert("vtx_projlod", "gl_Position = p + texture2DProjLod(s, q.xyz, 1.0);"))
    return out


def flow():
    out = []
    ops = {"gt": ">", "lt": "<", "ge": ">=", "le": "<=", "eq": "==", "ne": "!="}
    for n, o in ops.items():
        out.append(frag(f"fl_{n}", f"if (a.x {o} b.x) gl_FragColor = c; else gl_FragColor = d;"))
        out.append(frag(f"fl_{n}0", f"if (a.x {o} 0.0) gl_FragColor = c; else gl_FragColor = d;"))
        out.append(frag(f"fl_{n}_m", f"if (a.x {o} b.x) gl_FragColor = c; else gl_FragColor = d;", "mediump"))
        out.append(frag(f"fl_{n}_l", f"if (a.x {o} b.x) gl_FragColor = c; else gl_FragColor = d;", "lowp"))
        out.append(frag(f"fl_{n}_y", f"if (a.y {o} b.z) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("fl_and", "if (a.x > b.x && a.y > b.y) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("fl_or", "if (a.x > b.x || a.y > b.y) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("fl_not", "if (!(a.x > b.x)) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("fl_any", "if (any(greaterThan(a, b))) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("fl_all", "if (all(greaterThan(a, b))) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("fl_bu", "if (bu) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("fl_nested", "vec4 r = d; if (a.x > b.x) { r = c * d; if (a.y > b.y) r = r * a; "
                    "else r = r + b; } gl_FragColor = r;"))
    out.append(frag("fl_big", "vec4 r = d; if (a.x > b.x) { r = sin(c) * d + exp2(a) * log2(b) / c; "
                    "r = r * r + sqrt(a); } else { r = cos(d) * a; } gl_FragColor = r;"))
    out.append(frag("fl_sel", "gl_FragColor = (a.x > b.x) ? c : d;"))
    out.append(frag("fl_vsel", "gl_FragColor = mix(c, d, vec4(greaterThan(a, b)));"))
    # discard
    out.append(frag("dc_always", "discard;"))
    out.append(frag("dc_gt", "if (a.x > b.x) discard; gl_FragColor = c;"))
    out.append(frag("dc_lt0", "if (a.x < 0.0) discard; gl_FragColor = c;"))
    out.append(frag("dc_eq", "if (a.x == b.x) discard; gl_FragColor = c;"))
    out.append(frag("dc_tex", "vec4 t = texture2D(s, a.xy); if (t.w < 0.5) discard; gl_FragColor = t;"))
    out.append(frag("dc_tex_m", "vec4 t = texture2D(s, a.xy); if (t.w < 0.5) discard; gl_FragColor = t;",
                    "mediump"))
    out.append(frag("dc_late", "vec4 r = a * b; if (r.x > c.x) discard; gl_FragColor = r + d;"))
    # loops
    out.append(frag("lp_s4", "vec4 r = vec4(0.0); for (int i = 0; i < 4; i++) r += a * b; gl_FragColor = r;"))
    out.append(frag("lp_s32", "vec4 r = c; for (int i = 0; i < 32; i++) r = r * a + b; gl_FragColor = r;"))
    out.append(frag("lp_s100", "vec4 r = c; for (int i = 0; i < 100; i++) r = r * a + b; gl_FragColor = r;"))
    out.append(frag("lp_brk", "vec4 r = vec4(0.0); for (int i = 0; i < 16; i++) { if (r.x > a.x) break; "
                    "r += b; } gl_FragColor = r;"))
    out.append(frag("lp_cont", "vec4 r = vec4(0.0); for (int i = 0; i < 16; i++) { if (r.x > a.x) continue; "
                    "r += b; } gl_FragColor = r;"))
    out.append(frag("lp_n", "vec4 r = c; for (int i = 0; i < 64; i++) { if (i >= n) break; r = r * a + b; } "
                    "gl_FragColor = r;"))
    out.append(frag("lp_while", "vec4 r = c; int i = 0; while (r.x < a.x) { r += b; i++; if (i > 50) break; } "
                    "gl_FragColor = r;"))
    out.append(frag("lp_nest", "vec4 r = c; for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) "
                    "r = r * a + b; gl_FragColor = r;"))
    out.append(frag("lp_fi", "vec4 r = c; for (float x = 0.0; x < 10.0; x += 1.0) r = r * a + x; "
                    "gl_FragColor = r;"))
    out.append(frag("fn_call", "gl_FragColor = g(a, b) + g(c, d);",
                    decl="vec4 g(vec4 x, vec4 y) { if (x.x > y.x) return x * y; return x + y; }\n"))
    # derivatives and other fragment-only ops
    ext = "#extension GL_OES_standard_derivatives : enable\n"
    for n, e in {"dsx": "dFdx(a)", "dsy": "dFdy(a)", "fwidth": "fwidth(a)"}.items():
        out.append(frag(n, f"gl_FragColor = {e};").replace("precision", ext + "precision", 1))
    return out


def integer():
    out = []
    ib = "ivec4 i = ivec4(a); ivec4 j = ivec4(b);"
    for name, e in {"add": "i + j", "sub": "i - j", "mul": "i * j", "div": "i / j", "neg": "-i",
                    "addk": "i + 3", "mulk": "i * 5", "divk": "i / 4", "mulv": "i * n"}.items():
        out.append(frag(f"in_{name}", f"{ib} gl_FragColor = vec4({e});"))
        out.append(frag(f"in_{name}_m", f"{ib} gl_FragColor = vec4({e});", "mediump"))
        out.append(frag(f"is_{name}", f"int i = int(a.x); int j = int(b.x); int nn = n; "
                        f"gl_FragColor = vec4(float({e.replace('n', 'nn')}));"))
    out.append(frag("cv_ftoi", "gl_FragColor = vec4(ivec4(a));"))
    out.append(frag("cv_ftoi_s", "gl_FragColor = vec4(float(int(a.x)));"))
    out.append(frag("cv_itof_u", "gl_FragColor = vec4(float(n));"))
    out.append(frag("cv_itof", "gl_FragColor = vec4(float(n * int(a.x)));"))
    out.append(frag("cv_btof", "gl_FragColor = vec4(float(a.x > b.x));"))
    out.append(frag("cv_ftob", "gl_FragColor = vec4(bvec4(a));"))
    out.append(frag("cv_ftoi_m", "gl_FragColor = vec4(ivec4(a));", "mediump"))
    for n, o in {"lt": "<", "le": "<=", "gt": ">", "ge": ">=", "eq": "==", "ne": "!="}.items():
        out.append(frag(f"ic_{n}", f"int i = int(a.x); int j = int(b.x); "
                        f"if (i {o} j) gl_FragColor = c; else gl_FragColor = d;"))
    out.append(frag("ic_eqv", "gl_FragColor = vec4(equal(ivec4(a), ivec4(b)));"))
    out.append(frag("ic_ltv", "gl_FragColor = vec4(lessThan(ivec4(a), ivec4(b)));"))
    out.append(frag("bo_and", "bool x = a.x > b.x; bool y = a.y > b.y; gl_FragColor = vec4(x && y);"))
    out.append(frag("bo_or", "bool x = a.x > b.x; bool y = a.y > b.y; gl_FragColor = vec4(x || y);"))
    out.append(frag("bo_xor", "bool x = a.x > b.x; bool y = a.y > b.y; gl_FragColor = vec4(x ^^ y);"))
    out.append(frag("bo_not", "bvec4 x = greaterThan(a, b); gl_FragColor = vec4(not(x));"))
    out.append(frag("bo_bu", "gl_FragColor = vec4(bu && a.x > b.x);"))
    out.append(frag("in_mod", "int i = int(a.x); gl_FragColor = vec4(float(i - (i / 7) * 7));"))
    return out


def index():
    out = []
    d = "uniform vec4 ua[16]; uniform mat4 mm;\n"
    out.append(frag("ix_k", "gl_FragColor = a * ua[3];", decl=d))
    out.append(frag("ix_n", "gl_FragColor = a * ua[n];", decl=d))
    out.append(frag("ix_v", "gl_FragColor = a * ua[int(b.x)];", decl=d))
    out.append(frag("ix_v1", "gl_FragColor = a * ua[int(b.x) + 1];", decl=d))
    out.append(frag("ix_loop", "vec4 r = vec4(0.0); for (int i = 0; i < 16; i++) r += ua[i] * a; "
                    "gl_FragColor = r;", decl=d))
    out.append(frag("ix_comp", "vec4 r = a; r[int(b.x)] = 1.0; gl_FragColor = r;"))
    out.append(frag("ix_rcomp", "gl_FragColor = vec4(a[int(b.x)]);"))
    out.append(frag("ix_tmparr", "vec4 t[4]; t[0] = a; t[1] = b; t[2] = c; t[3] = d; "
                    "gl_FragColor = t[int(a.x)];"))
    out.append(frag("ix_tmpw", "vec4 t[4]; t[0] = a; t[1] = b; t[2] = c; t[3] = d; t[int(a.y)] = u; "
                    "gl_FragColor = t[int(a.x)];"))
    out.append(vert("vix_k", "gl_Position = p * ka[2];"))
    out.append(vert("vix_n", "gl_Position = p * ka[n];"))
    out.append(vert("vix_a", "gl_Position = p * ka[int(pi)];"))
    out.append(vert("vix_skin", "gl_Position = mb[int(pi)] * p;"))
    out.append(vert("vix_loop", "vec4 t = vec4(0.0); for (int i = 0; i < 8; i++) t += ka[i] * p; gl_Position = t;"))
    return out


def vertex():
    out = []
    v = {
        "vo_pos": "gl_Position = p;",
        "vo_posk": "gl_Position = vec4(1.0, 2.0, 3.0, 1.0);",
        "vo_mvp": "gl_Position = m * p;",
        "vo_v0": "gl_Position = p; o0 = q;",
        "vo_v01": "gl_Position = p; o0 = q; o1 = r;",
        "vo_v2": "gl_Position = p; o2 = q.xy;",
        "vo_v3": "gl_Position = p; o3 = q.x;",
        "vo_vall": "gl_Position = p; o0 = q; o1 = r; o2 = q.zw; o3 = r.y;",
        "vo_vm": "gl_Position = p; om = q;",
        "vo_ps": "gl_Position = p; gl_PointSize = q.x;",
        "vo_psk": "gl_Position = p; gl_PointSize = 4.0;",
        "vo_psv": "gl_Position = p; gl_PointSize = q.x; o0 = r;",
        "vo_mvpv": "gl_Position = m * p; o0 = m * q; o2 = r.xy;",
        "vo_if": "gl_Position = p; if (q.x > 0.0) o0 = q; else o0 = r;",
        "vo_loop": "vec4 t = p; for (int i = 0; i < n; i++) t = t * q; gl_Position = t;",
        "vo_vary_f": "gl_Position = p; o0 = q * f;",
    }
    for n, b in v.items():
        out.append(vert(n, b))
    # varyings read in a fragment shader: precision and count
    for n, b in {"vi_1": "gl_FragColor = a;", "vi_2": "gl_FragColor = a * b;",
                 "vi_4": "gl_FragColor = a * b + c * d;"}.items():
        out.append(frag(n, b))
    return out


def main():
    out = texture() + flow() + integer() + index() + vertex()
    print("".join(out), end="")


if __name__ == "__main__":
    main()
