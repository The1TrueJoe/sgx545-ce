"""glamor's shaders as a corpus file (run_corpus.py input), built from an X server
checkout the way glamor builds them on desktop GL 2.1 (GLSL 1.10/1.20, the
programs without the GLSL 1.30 facets).

    python3 -I tools/usc/glamor_corpus.py XSERVER_CHECKOUT OUT.glsl

- glamor_render.c's composite programs: every source (solid, alpha texture,
  texture) x mask (none, solid, alpha texture, texture) x combine (normal, CA
  source, CA alpha), default destination swizzle.
- glamor_program.c's facet programs: the 1.20 rectangle prim with each fill
  (solid, tile, stipple, opaque stipple), the copy-area prim, and the 1.20
  composite-glyphs prim with each render source and combine.

The corpus is GLSL ES 1.00, so each fragment shader gets
`precision highp float;` (desktop GL has no precision, so everything is
highp). Generated, never committed: the strings are the X server's.
"""
import re
import sys

STR = re.compile(r'"((?:[^"\\]|\\.)*)"')


def cstr(text):
    """the C string literals in text, concatenated and unescaped"""
    s = "".join(STR.findall(text))
    return s.encode().decode("unicode_escape")


def macros(src):
    """GLAMOR_POS / GLAMOR_DECLARE_MATRIX / GLAMOR_DEFAULT_PRECISION as strings"""
    def pos(m):
        dst, srcv = m.group(1).strip(), m.group(2).strip()
        return ('"       %s.xy = %s.xy * v_matrix.xz + v_matrix.yw;\\n"'
                '"       %s.zw = vec2(0.0,1.0);\\n"' % (dst, srcv, dst))
    src = re.sub(r"GLAMOR_POS\((\w[\w.]*),\s*(\([^()]*\)|[\w.]+)\)", pos, src)
    src = src.replace("GLAMOR_DECLARE_MATRIX", '"uniform vec4 v_matrix;\\n"')
    src = src.replace("GLAMOR_DEFAULT_PRECISION", '""')
    return src


def var(src, name):
    m = re.search(r"const char \*%s\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\")+)\s*;" % name, src)
    if not m:
        sys.exit(f"glamor_corpus: {name} not found")
    return cstr(m.group(1))


def facet(src, name):
    """a glamor_facet initializer's string fields"""
    m = re.search(r"glamor_facet %s\s*=\s*\{(.*?)\n\};" % name, src, re.S)
    if not m:
        sys.exit(f"glamor_corpus: facet {name} not found")
    body = m.group(1)
    out = {}
    for f in ("vs_vars", "vs_exec", "fs_vars", "fs_exec"):
        fm = re.search(r"\.%s\s*=\s*(\(?(?:\s*\"(?:[^\"\\]|\\.)*\")+\s*\)?)" % f, body)
        out[f] = cstr(fm.group(1)) if fm else ""
    loc = re.search(r"\.locations\s*=\s*([^,\n]+)", body)
    out["locations"] = loc.group(1) if loc else ""
    return out


LOCATION_VARS = [
    ("glamor_program_location_fg", "", "uniform vec4 fg;\n"),
    ("glamor_program_location_bg", "", "uniform vec4 bg;\n"),
    ("glamor_program_location_fillsamp", "", "uniform sampler2D sampler;\n"),
    ("glamor_program_location_fillpos",
     "uniform vec2 fill_offset;\nuniform vec2 fill_size_inv;\nvarying vec2 fill_pos;\n",
     "varying vec2 fill_pos;\n"),
    ("glamor_program_location_atlas", "", "uniform sampler2D atlas;\n"),
]


def build(prim, fill, combine="", defines=""):
    """glamor_build_program's templates (no #version: GLSL 1.20 facets)"""
    locs = prim["locations"] + "|" + fill.get("locations", "")
    vsv = "".join(v for l, v, _ in LOCATION_VARS if l in locs)
    fsv = "".join(f for l, _, f in LOCATION_VARS if l in locs)
    vs = (defines + prim["vs_vars"] + fill.get("vs_vars", "") + vsv +
          "uniform vec4 v_matrix;\nvoid main() {\n" + prim["vs_exec"] + fill.get("vs_exec", "") + "}\n")
    fs = ("precision highp float;\n" + defines + prim["fs_vars"] + fill.get("fs_vars", "") + fsv +
          "void main() {\n" + prim["fs_exec"] + fill.get("fs_exec", "") + combine + "}\n")
    return vs, fs


def main():
    root, out = sys.argv[1], sys.argv[2]
    g = root.rstrip("/") + "/glamor/"
    render = open(g + "glamor_render.c").read()
    prog = macros(open(g + "glamor_program.c").read())
    copy = macros(open(g + "glamor_copy.c").read())
    rects = macros(open(g + "glamor_rects.c").read())
    glyphs = macros(open(g + "glamor_composite_glyphs.c").read())
    progs = []

    # composite (glamor_render.c)
    head = "precision highp float;\n" + "".join(
        var(render, n) for n in ("repeat_define", "relocate_texture", "rel_sampler"))
    sources = {"solid": "source_solid_fetch", "alpha": "source_alpha_pixmap_fetch",
               "tex": "source_pixmap_fetch"}
    masks = {"none": "mask_none", "solid": "mask_solid_fetch",
             "alpha": "mask_alpha_pixmap_fetch", "tex": "mask_pixmap_fetch"}
    ins = {"n": "in_normal", "cas": "in_ca_source", "caa": "in_ca_alpha"}
    vs_open = var(render, "main_opening")
    for sk, sv in sources.items():
        for mk, mv in masks.items():
            vs = vs_open
            if sk != "solid":
                vs += var(render, "source_coords")
            if mk not in ("none", "solid"):
                vs += var(render, "mask_coords")
            vs += var(render, "main_closing")
            for ik, iv in ins.items():
                if mk in ("none", "solid") and ik != "n":
                    continue
                fs = head + var(render, sv) + var(render, mv) + var(render, "dest_swizzle_default") + var(render, iv)
                progs.append((f"comp_{sk}_{mk}_{ik}", vs, fs))

    # facet programs (glamor_program.c, glamor_copy.c, glamor_rects.c, glyphs)
    rect = facet(rects, "glamor_facet_polyfillrect_120")
    for f in ("glamor_fill_solid", "glamor_fill_tile", "glamor_fill_stipple", "glamor_fill_opaque_stipple"):
        vs, fs = build(rect, facet(prog, f))
        progs.append(("rect_" + f.split("glamor_fill_")[1], vs, fs))
    vs, fs = build(facet(copy, "glamor_facet_copyarea"), {})
    progs.append(("copy_area", vs, fs))
    gl = facet(glyphs, "glamor_facet_composite_glyphs_120")
    combine = re.search(r"glamor_combine\[\]\s*=\s*\{(.*?)\};", prog, re.S).group(1)
    combs = {}
    for k, v in re.findall(r"\[glamor_program_alpha_(\w+)\]\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\")+)", combine):
        combs[k] = cstr(v)
    for s in ("glamor_source_solid", "glamor_source_picture", "glamor_source_1x1_picture"):
        src = facet(prog, s)
        for ck in ("normal", "ca_first", "ca_second"):
            vs, fs = build(gl, src, combs[ck], "#define ATLAS_DIM_INV 0.0009765625\n")
            progs.append((f"glyph_{s.split('glamor_source_')[1]}_{ck}", vs, fs))

    with open(out, "w") as o:
        o.write("# glamor (X server %s), generated by tools/usc/glamor_corpus.py\n" % root)
        for name, vs, fs in progs:
            o.write(f"@vert {name}\n{vs}\n@frag {name}\n{fs}\n")
    print(f"{len(progs)} programs")


if __name__ == "__main__":
    main()
