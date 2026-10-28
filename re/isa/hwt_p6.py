"""hwt.py suite, priority 5: sop2 on U8 and C10 operands, with and without setfc (format control)."""
from hwt_lib import load, out

TESTS = []
SEL = "c1=one c2=zero cop=add a1=zero a2=one aop=add"     # rgb = src1, a = src2 (the corpus's lowp select)
U = {"r0": 0x11335599, "r1": 0xaabbccdd}
C10 = {"r0": 0x100 | 0x080 << 10 | 0x040 << 20, "r1": 0x0ff | 0x3ff << 10 | 0x1fe << 20}
for fc in ("", "setfc #1, #1", "setfc #0, #0", "setfc #1, #0", "setfc #0, #1"):
    pre = [fc] if fc else []
    tag = fc.replace("setfc #", "fc").replace(", #", "") or "nofc"
    TESTS.append((f"sop2_u8_sel_{tag}", load(U) + pre + [f"sop2 r3, r0, r1  [skipinv {SEL}]", out("r3")], 0xaa335599))
    TESTS.append((f"sop2_c10_sel_{tag}", load(C10) + pre + [f"sop2 r3, r0.c10, r1.c10  [skipinv {SEL}]", out("r3")], None))
# U8 arithmetic: per-byte saturating add, multiply by the other source
TESTS.append(("sop2_u8_add", load({"r0": 0x40302010, "r1": 0xf0a08070})
              + ["sop2 r3, r0, r1  [skipinv c1=one c2=one cop=add a1=one a2=one aop=add]", out("r3")], None))
TESTS.append(("sop2_u8_sub", load({"r0": 0x40302010, "r1": 0x10a00870})
              + ["sop2 r3, r0, r1  [skipinv c1=one c2=one cop=sub a1=one a2=one aop=sub]", out("r3")], None))
TESTS.append(("sop2_u8_mul", load({"r0": 0x80808080, "r1": 0x80ff4000})
              + ["sop2 r3, r0, r1  [skipinv c1=s2 c2=zero cop=add a1=s2a a2=zero aop=add]", out("r3")], None))
TESTS.append(("sop2_c10_add", load(C10) + ["setfc #1, #1",
              "sop2 r3, r0.c10, r1.c10  [skipinv c1=one c2=one cop=add a1=one a2=one aop=add]", out("r3")], None))
