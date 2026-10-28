"""Compare gl4es ctest snapshots from the EA3 (mesa/tests/gl4es-ctest-ab.sh).

    python3 ctest_cmp.py OUT_DIR REFS_DIR [--json out.json]

OUT_DIR holds <who>-<test>-es<N>-<call>.png for who = ddk and ours; REFS_DIR is
gl4es' refs/. For every test:
  - ours against the DDK, pixel for pixel: pixels that differ at all, pixels
    off by more than 20%, and the largest channel difference;
  - each against gl4es' reference the way gl4es' tests/test.sh does
    (ImageMagick `compare -metric AE -fuzz 20%` over its extract region,
    pass when under the test's tolerance).
Needs Pillow.
"""
import json
import os
import sys

from PIL import Image, ImageChops

# test: (call, {es: tolerance}, extract "WxH+X+Y" or None), from gl4es' CMakeLists.txt
TESTS = {
    "glxgears": ("0000008203", {1: 25, 2: 700}, None),
    "stuntcarracer": ("0000118817", {1: 20, 2: 20}, "638x478+1+1"),
    "neverball": ("0000078750", {1: 20, 2: 200}, "798x478+1+1"),
    "foobillardplus": ("0000014748", {1: 50, 2: 60}, "798x478+1+1"),
    "descent3": ("0000284192", {1: 20, 2: 20}, "638x478+1+1"),
    "pointsprite": ("0000248810", {1: 20, 2: 20}, None),
    "openra": ("0000031249", {2: 20}, "638x478+1+1"),
    "glsl_lighting": ("0000505393", {2: 20}, None),
}
FUZZ = 0.20


def load(path):
    return Image.open(path).convert("RGB") if os.path.exists(path) else None


def crop(im, extract):
    if not extract or im is None:
        return im
    wh, x, y = extract.split("+")
    w, h = (int(v) for v in wh.split("x"))
    return im.crop((int(x), int(y), int(x) + w, int(y) + h))


def far(p):
    # ImageMagick's fuzz distance for RGB: root mean square of the channel differences
    return (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) / 3 > (FUZZ * 255) ** 2


def diff(a, b):
    if a is None or b is None:
        return None
    if a.size != b.size:
        return {"size": [a.size, b.size]}
    d = list(ImageChops.difference(a, b).get_flattened_data())
    return {"differ": sum(1 for p in d if p != (0, 0, 0)), "far": sum(1 for p in d if far(p)),
            "max": max(max(p) for p in d)}


def main(out, refs, js=None):
    rows = {}
    for name, (call, tol, extract) in TESTS.items():
        ref = load(os.path.join(refs, f"{name}.{call}.png"))
        for es in sorted(tol):
            r = {}
            im = {w: load(os.path.join(out, f"{w}-{name}-es{es}-{call}.png")) for w in ("ddk", "ours")}
            r["ours_vs_ddk"] = diff(im["ours"], im["ddk"])
            for w in ("ddk", "ours"):
                d = diff(crop(ref, extract), crop(im[w], extract))
                r[f"{w}_vs_ref"] = d
                r[f"{w}_ctest"] = None if not d or "far" not in d else d["far"] < tol[es]
            r["tolerance"] = tol[es]
            rows[f"{name} es{es}"] = r
            a, dr, orf = r["ours_vs_ddk"], r["ddk_vs_ref"], r["ours_vs_ref"]

            def fmt(d):
                if d is None:
                    return "no snapshot"
                if "size" in d:
                    return f"size {d['size']}"
                return f"{d['differ']} px differ, {d['far']} >20%, max {d['max']}"

            def ok(v):
                return "-" if v is None else "pass" if v else "FAIL"

            print(f"{name:15} es{es}  ours vs DDK: {fmt(a):38}  ctest DDK {ok(r['ddk_ctest']):4} "
                  f"({dr['far'] if dr and 'far' in dr else '-'}) ours {ok(r['ours_ctest']):4} "
                  f"({orf['far'] if orf and 'far' in orf else '-'}) / tol {tol[es]}")
    if js:
        with open(js, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    js = sys.argv[sys.argv.index("--json") + 1] if "--json" in sys.argv else None
    if js in args:
        args.remove(js)
    main(args[0], args[1], js)
