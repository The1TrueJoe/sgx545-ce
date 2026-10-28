"""hwt.py suite, priority 4: fminmax/fmaxmin, fmad16 swizzles, pck lane order and rounding."""
import itertools

from hwt_lib import F, H, load, out

TESTS = []

# min/max: every permutation of (1, 2, 3) over (s0, s1, s2)
for name in ("fminmax", "fmaxmin"):
    for p in itertools.permutations((1, 2, 3)):
        TESTS.append((f"{name}_{''.join(map(str, p))}", load({"r0": F(p[0]), "r1": F(p[1]), "r2": F(p[2])})
                      + [f"{name} r3, r0, r1, r2  [skipinv]", out("r3")], None))

# fmad16 swizzles: r0 = (2, 3), r1 = (5, 7), r2 = (0.25, 0.5) as (lo, hi); unswizzled = (10.25, 21.5)
FH = {"r0": H(2, 3), "r1": H(5, 7), "r2": H(0.25, 0.5)}
TESTS.append(("fmad16_base", load(FH) + ["fmad16 r3, r0, r1, r2  [skipinv]", out("r3")], H(10.25, 21.5)))
for s in range(3):
    for v in (1, 2, 3):
        TESTS.append((f"fmad16_swz{s}_{v}", load(FH) + [f"fmad16 r3, r0, r1, r2  [skipinv swz{s}={v}]", out("r3")], None))
        # the same operand as an f32 1.5 (a CVTFROMF32 swizzle would read it as such)
        TESTS.append((f"fmad16_swz{s}_{v}_f32", load({**FH, f"r{s}": F(1.5)})
                      + [f"fmad16 r3, r0, r1, r2  [skipinv swz{s}={v}]", out("r3")], None))

# pck lane order: r0 = 0.2 (-> 51 = 0x33), r1 = 0.6 (-> 153 = 0x99); r3 starts 0x5a5a5a5a
P = {"r0": F(0.2), "r1": F(0.6), "r3": 0x5a5a5a5a}
for dm in (1, 2, 3, 5, 6, 9, 12, 15):
    TESTS.append((f"pck_u8_dmask{dm}", load(P) + [f"pck.u8.f32 r3, r0, r1  [skipinv dmask={dm} scale]", out("r3")], None))
# u8 scale rounding: 0.5 -> 127.5, 0.3 -> 76.5, 1/510 -> 0.5, -0.1 and 1.5 (clamp)
for x in (0.5, 0.3, 1 / 510, -0.1, 1.5, 0.7):
    for rnd in ("", " rnd=0", " rnd=1", " rnd=2", " rnd=3"):
        TESTS.append((f"pck_u8_{x:.4g}{rnd.replace(' ', '_')}", load({"r0": F(x), "r3": 0})
                      + [f"pck.u8.f32 r3, r0, #0  [skipinv dmask=1 scale{rnd}]", out("r3")], None))
# float -> float round to integral
for x in (2.5, -2.5, 1.7, -1.7, 3.5, 0.5):
    for rnd in range(4):
        TESTS.append((f"rint_{x:g}_rnd{rnd}", load({"r0": F(x)}) + [f"pck.f32.f32 r3, r0, #0  [skipinv rint rnd={rnd}]", out("r3")], None))
# f32 -> f16 rounding: 1 + 2^-11 (a tie), 1 + 3 * 2^-12, 1/3
for x in (1 + 2 ** -11, 1 + 3 * 2 ** -12, 1 / 3, -(1 + 3 * 2 ** -12)):
    for rnd in range(4):
        TESTS.append((f"f16_{x:.6g}_rnd{rnd}", load({"r0": F(x), "r3": 0})
                      + [f"pck.f16.f32 r3, r0, #0  [skipinv dmask=3 rnd={rnd}]", out("r3")], None))
# f32 -> s16, two sources (which half each lands in) and saturation
TESTS.append(("pck_s16_2src", load({"r0": F(5), "r1": F(-7), "r3": 0x5a5a5a5a})
              + ["pck.s16.f32 r3, r0, r1  [skipinv]", out("r3")], None))
for x in (2.5, -2.5, 70000, -70000, 1.7):
    TESTS.append((f"pck_s16_{x:g}", load({"r0": F(x), "r3": 0x5a5a5a5a})
                  + ["pck.s16.f32 r3, r0, #0  [skipinv dmask=3]", out("r3")], None))
# unpack: lane select on 8-bit sources, r0 = 0x11335599
for ln in range(4):
    TESTS.append((f"unpck_u8_l{ln}", load({"r0": 0x11335599})
                  + [f"pck.f32.u8 r3, r0.l{ln}, #0  [skipinv scale]" if ln else "pck.f32.u8 r3, r0, #0  [skipinv scale]",
                     out("r3")], None))
    TESTS.append((f"unpck_u8_l{ln}_noscale", load({"r0": 0x11335599})
                  + [f"pck.f32.u8 r3, r0.l{ln}, #0  [skipinv]" if ln else "pck.f32.u8 r3, r0, #0  [skipinv]",
                     out("r3")], None))
