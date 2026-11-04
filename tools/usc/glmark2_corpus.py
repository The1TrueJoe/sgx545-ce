"""glmark2's shaders as a corpus file (run_corpus.py / shc input).

    python3 -I tools/usc/glmark2_corpus.py GLMARK2_CHECKOUT OUT.glsl

As glmark2's ES2 build compiles them: fragment shaders get `precision mediump
float;`, HIGHP_OR_DEFAULT is highp, and the constants its scenes add with
add_const() are declared with the scenes' defaults. Shaders with $PLACEHOLDER$
templates (conditionals, function, loop, effect2d, desktop) are left out.
glmark2's shaders are GPL: the corpus is generated, never committed.
"""
import os
import re
import sys

CONST = {
    "LightSourcePosition": "const vec4 LightSourcePosition = vec4(20.0, 20.0, 10.0, 1.0);",
    "LightSourceHalfVector": "const vec3 LightSourceHalfVector = vec3(0.408248, 0.408248, 0.816497);",
    "LightColor": "const vec4 LightColor = vec4(0.8, 0.8, 0.8, 1.0);",
    "MaterialDiffuse": "const vec4 MaterialDiffuse = vec4(0.7, 0.7, 0.7, 1.0);",
    "PI": "const float PI = 3.14159265;",
    "RefractiveIndex": "const float RefractiveIndex = 1.2;",
    "TextureSize": "const vec2 TextureSize = vec2(512.0, 512.0);",
    "TextureStepX": "const float TextureStepX = 0.0013888889;",
    "TextureStepY": "const float TextureStepY = 0.0020833333;",
    "TiltShift": "const float TiltShift = 0.0;",
}


def main():
    d = os.path.join(sys.argv[1], "data", "shaders")
    out = []
    for f in sorted(os.listdir(d)):
        if not f.endswith((".vert", ".frag")):
            continue
        src = open(os.path.join(d, f)).read()
        if "$" in src:
            continue
        kind = "vert" if f.endswith(".vert") else "frag"
        hdr = ("precision mediump float;\n" if kind == "frag" else "") + "#define HIGHP_OR_DEFAULT highp\n"
        for k, v in CONST.items():
            if re.search(r"\b%s\b" % k, src) and not re.search(r"(uniform|const)\s+\w+\s+%s\b" % k, src):
                hdr += v + "\n"
        out.append(f"@{kind} {re.sub(r'[^a-z0-9]', '_', f[:-5].lower())}\n{hdr}{src}\n")
    open(sys.argv[2], "w").write("".join(out))
    print(f"{len(out)} shaders")


if __name__ == "__main__":
    main()
