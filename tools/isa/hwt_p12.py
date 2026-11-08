"""hwt.py suite: what efo's units and fdual's secondary op compute (ISA.md "efo", "Dual issue"),
before the compiler co-issues with them. Sources and internal registers hold distinct values
(s0 = 2, s1 = 3, s2 = 5; i0 = 7, i1 = 11, i2 = 13), so every sum or product names its inputs."""
from hwt_lib import F, load, out

TESTS = []
M1 = "[skipinv mask=1]"
SRC = {"r0": F(2.0), "r1": F(3.0), "r2": F(5.0), "r4": F(7.0), "r5": F(11.0), "r6": F(13.0),
       "r3": 0x5a5a5a5a}
# (internal registers don't survive a deschedule: nosched from the first write of one to its read)
NS = "[skipinv nosched mask=1]"
PRE = load(SRC) + [f"mov i0, r4  {NS}", f"mov i1, r5  {NS}", f"mov i2, r6  {NS}"]

# efo multiplier modes: products into I0/I1 (i=i0=m0,i1=m1), each read back
M = {0: "m0=s0*s2,m1=s1*s2", 1: "m0=s2*s1,m1=s2*s2", 2: "m0=s1*s0,m1=s2*s2", 3: "m0=s1*i0,m1=s2*i1"}
for m, txt in M.items():
    for ir in ("i0", "i1"):
        TESTS.append((f"efo_m{m}_{ir}", PRE + [f"efo r3, r0, r1, r2  [skipinv nosched d=i0 wi0 wi1 i=i0=m0,i1=m1 m={txt}]",
                                              f"mov r3, {ir}  {M1}", out("r3")], None))
# efo adder modes: sums into I0/I1 (i=i0=a0,i1=a1), products m mode 0 (m0 = 10, m1 = 15)
A = {0: "a0=m0+m1,a1=i1+i0", 1: "a0=m0+s0,a1=i1+i0", 2: "a0=m0+i0,a1=i1+m1", 3: "a0=s0+s2,a1=s1+s2"}
for a, txt in A.items():
    for ir in ("i0", "i1"):
        TESTS.append((f"efo_a{a}_{ir}", PRE + [f"efo r3, r0, r1, r2  [skipinv nosched d=i0 wi0 wi1 i=i0=a0,i1=a1 a={txt} m=m0=s0*s2,m1=s1*s2]",
                                              f"mov r3, {ir}  {M1}", out("r3")], None))
# efo's destination select and the remaining i modes
for d in ("i0", "i1", "a0", "a1"):
    TESTS.append((f"efo_d_{d}", PRE + [f"efo r3, r0, r1, r2  [skipinv nosched d={d} i=i0=a1,i1=a0 a=a0=s0+s2,a1=s1+s2]", out("r3")], None))
for i, txt in ((1, "i0=a1,i1=a0"), (3, "i0=a0,i1=m1")):
    for ir in ("i0", "i1"):
        TESTS.append((f"efo_i{i}_{ir}", PRE + [f"efo r3, r0, r1, r2  [skipinv nosched d=i0 wi0 wi1 i={txt} a=a0=s0+s2,a1=s1+s2 m=m0=s0*s2,m1=s1*s2]",
                                              f"mov r3, {ir}  {M1}", out("r3")], None))
# without wi0 / wi1 the internal registers keep their values
TESTS.append(("efo_nowi_i0", PRE + ["efo r3, r0, r1, r2  [skipinv nosched d=a0 i=i0=m0,i1=m1]", f"mov r3, i0  {M1}", out("r3")], F(7.0)))
# a0's negates: a0 = s0 + s2 = 7 with each flag
for flag in ("a0rneg", "a0lneg"):
    TESTS.append((f"efo_{flag}", PRE + [f"efo r3, r0, r1, r2  [skipinv nosched d=a0 a=a0=s0+s2,a1=s1+s2 {flag}]", out("r3")], None))

# fdual: the primary a plain mov (r3 = s1 slot), the secondary into sd, read back from it
SEC1 = ("frcp", "frsq", "fexp2", "flog2", "fsin", "fcos", "mov")
SEC2 = ("fadd", "fsub", "fmul", "fmaxmin")
SEC3 = ("fmad", "fmsa", "fadm")
for sec in SEC2 + SEC3:
    for sb in ("i1", "src1"):
        for sc in ("i2", "src2"):
            TESTS.append((f"fdual_{sec}_sb{sb}_sc{sc}", PRE + [
                f"fdual.mov.{sec} r3, r0, r1, r2  [skipinv nosched sd=i0 sb={sb} sc={sc}]", f"mov r3, i0  {M1}", out("r3")], None))
for sec in SEC1:
    sel = "msrc=i1" if sec == "mov" else "sb=i1"
    TESTS.append((f"fdual_{sec}", PRE + [f"fdual.mov.{sec} r3, r0, r1, r2  [skipinv nosched sd=i0 {sel}]",
                                         f"mov r3, i0  {M1}", out("r3")], None))
# sd = i0s0: the secondary's first source from the src0 slot
TESTS.append(("fdual_fmad_i0s0", PRE + ["fdual.mov.fmad r3, r0, r1, r2  [skipinv nosched sd=i0s0 sb=i1 sc=i2]",
                                        f"mov r3, i0  {M1}", out("r3")], None))
# the primary's own result alongside a secondary
for pri in ("fmad", "fadm", "fmsa", "fminmax"):
    TESTS.append((f"fdual_pri_{pri}", PRE + [f"fdual.{pri}.fadd r3, r0, r1, r2  [skipinv nosched sd=i0 sb=i1 sc=i2]", out("r3")], None))
