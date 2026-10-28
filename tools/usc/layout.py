"""The DDK's GLSL compiler (libglslcompiler 1.7) numbers uniform constants
and vertex attributes itself and keeps that numbering in its compiled
object, outside the USP. A USP from another compiler has to use the same
numbers for the DDK's driver to feed it, so this approximates them from the
source.

Constants (read off corpus c1's USP list C, tools/isa/corpus/c1): every uniform
and literal gets constants on its first appearance in main(), in source
order; vectors and matrices start at a multiple of 4, scalars take the next
free component. `1.0 / u` gives 1.0 constant 0 and u 4-7; `if (u.x > 0.5)
discard; gl_FragColor = v;` gives u 0-3, 0.5 4, v 8-11. Builtins expanded by
glslcompiler add constants of their own (exp: one, sin/cos: two) after their
argument; the backend doesn't read those.

Attributes (corpus c3 vo_*/vtx_*): slot k = pa 4k, in order of first use.
"""
import re

TYPES = {"float": 1, "int": 1, "bool": 1, "vec2": 2, "vec3": 3, "vec4": 4, "ivec2": 2, "ivec3": 3, "ivec4": 4,
         "bvec2": 2, "bvec3": 3, "bvec4": 4, "mat2": 8, "mat3": 12, "mat4": 16}
DECL = re.compile(r"\buniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;")
ATTR = re.compile(r"\battribute\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*;")
TOKEN = re.compile(r"[A-Za-z_]\w*|\d+\.\d*(?:e[-+]?\d+)?|\.\d+(?:e[-+]?\d+)?|\d+")
BUILTIN_CONSTS = {"exp": 1, "log": 1, "sin": 2, "cos": 2}


def body(src):
    i = src.find("void main")
    return src[i:] if i >= 0 else src


def uniforms(src):
    decl = {}
    for ty, name, n in DECL.findall(src):
        if ty.startswith("sampler"):
            continue
        size = TYPES.get(ty, 4)
        decl[name] = (size if not n else 4 * int(n) * max(1, size // 4) if size >= 4 else 4 * int(n), bool(n))
    out, nxt, seen = {}, 0, set()
    toks = TOKEN.findall(body(src))
    for i, t in enumerate(toks):
        if t in decl and t not in seen:
            size, arr = decl[t]
            if size >= 2 or arr:
                nxt = (nxt + 3) & ~3
            out[t] = nxt
            nxt += size
            seen.add(t)
        elif t[0].isdigit() or t[0] == ".":
            if t in seen:
                continue
            # vecN(literal) constructors take a whole vector
            if i >= 1 and toks[i - 1] in ("vec2", "vec3", "vec4"):
                nxt = (nxt + 3) & ~3
                nxt += TYPES[toks[i - 1]]
            else:
                nxt += 1
            seen.add(t)
        elif t in BUILTIN_CONSTS:
            pass
    return out


def attributes(src):
    names = [n for _, n in ATTR.findall(src)]
    order = []
    for t in TOKEN.findall(body(src)):
        if t in names and t not in order:
            order.append(t)
    return {n: i for i, n in enumerate(order)}
