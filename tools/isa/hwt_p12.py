"""hwt.py suite: what efo's units and fdual's two ops compute, before the compiler co-issues with
them (ISA.md "efo", "Dual issue"). Sources and internal registers hold distinct values
(s0 = 2, s1 = 3, s2 = 5; i0 = 7, i1 = 17, i2 = 19), so every sum or product names its inputs."""
from hwt_lib import F, load, out

TESTS = []
M1 = "[skipinv mask=1]"
# (internal registers don't survive a deschedule: nosched from the first write of one to its read)
NS = "[skipinv nosched mask=1]"
SRC = {"r0": F(2.0), "r1": F(3.0), "r2": F(5.0), "r4": F(7.0), "r5": F(17.0), "r6": F(19.0),
       "r3": 0x5a5a5a5a}
PRE = load(SRC) + [f"mov i0, r4  {NS}", f"mov i1, r5  {NS}", f"mov i2, r6  {NS}"]


def reads(name, insn, regs):
    """one test per register read back after insn"""
    for r in regs:
        TESTS.append((f"{name}_{r}", PRE + [insn] + ([f"mov r3, {r}  {M1}"] if r != "r3" else []) + [out("r3")], None))


IR = ("i0", "i1", "i2")

# --- efo --------------------------------------------------------------------------------------
# multiplier modes: products into I0/I1 (i=i0=m0,i1=m1)
M = {0: "m0=s0*s2,m1=s1*s2", 1: "m0=s2*s1,m1=s2*s2", 2: "m0=s1*s0,m1=s2*s2", 3: "m0=s1*i0,m1=s2*i1"}
for m, txt in M.items():
    reads(f"efo_m{m}", f"efo r3, r0, r1, r2  [skipinv nosched d=a0 wi0 wi1 i=i0=m0,i1=m1 m={txt}]", ("i0", "i1"))
# adder modes: sums into I0/I1 (i=i0=a0,i1=a1); products m mode 0 (m0 = 10, m1 = 15)
A = {0: "a0=m0+m1,a1=i1+i0", 1: "a0=m0+s0,a1=i1+i0", 2: "a0=m0+i0,a1=i1+m1", 3: "a0=s0+s2,a1=s1+s2"}
for a, txt in A.items():
    reads(f"efo_a{a}", f"efo r3, r0, r1, r2  [skipinv nosched d=a0 wi0 wi1 i=i0=a0,i1=a1 a={txt} m=m0=s0*s2,m1=s1*s2]",
          ("i0", "i1"))
# the remaining i modes
for i, txt in ((1, "i0=a1,i1=a0"), (3, "i0=a0,i1=m1")):
    reads(f"efo_i{i}", f"efo r3, r0, r1, r2  [skipinv nosched d=a0 wi0 wi1 i={txt} a=a0=s0+s2,a1=s1+s2 m=m0=s0*s2,m1=s1*s2]",
          ("i0", "i1"))
# d: each select, without I writes, then with them: does d=i0 give I0's old value or the new one?
for d in ("i0", "i1", "a0", "a1"):
    reads(f"efo_d_{d}", f"efo r3, r0, r1, r2  [skipinv nosched d={d} i=i0=a1,i1=a0 a=a0=s0+s2,a1=s1+s2]", ("r3",))
    reads(f"efo_d_{d}_wi", f"efo r3, r0, r1, r2  [skipinv nosched d={d} wi0 wi1 i=i0=a1,i1=a0 a=a0=s0+s2,a1=s1+s2]", ("r3",))
# without wi0 / wi1 the internal registers keep their values
reads("efo_nowi", "efo r3, r0, r1, r2  [skipinv nosched d=a0 i=i0=m0,i1=m1]", ("i0", "i1"))
# a0's negates (a0 = s0 + s2 = 7 without)
for flag in ("a0rneg", "a0lneg"):
    reads(f"efo_{flag}", f"efo r3, r0, r1, r2  [skipinv nosched d=a0 a=a0=s0+s2,a1=s1+s2 {flag}]", ("r3",))
# chaining as v_mvp does: an efo reading the I0/I1 the previous one wrote
TESTS.append(("efo_chain_r3", PRE + [
    "efo r3, r0, r1, r2  [skipinv nosched d=a0 wi0 wi1 i=i0=m0,i1=m1 a=a0=s0+s2,a1=s1+s2 m=m0=s0*s2,m1=s1*s2]",
    "efo r3, r0, r1, r2  [skipinv nosched d=a0 wi0 wi1 i=i0=a0,i1=a1 a=a0=m0+i0,a1=i1+m1 m=m0=s0*s2,m1=s1*s2]",
    out("r3")], None))

# --- fdual: the secondary op ----------------------------------------------------------------
# a mov primary; the secondary with sd = i0, sb = i1, sc = i2; all three internal registers read back
SEC = ("fmad", "fmad16", "fmsa", "fadm", "fmaxmin", "frcp", "frsq", "fexp2", "flog2", "fsin", "fcos",
       "fadd", "fsub", "fmul", "ima32")
for sec in SEC:
    reads(f"fdual_sec_{sec}", f"fdual.mov.{sec} r3, r0, r1, r2  [skipinv nosched sd=i0 sb=i1 sc=i2]", IR)
for msrc in ("i1", "src0", "src1", "src2"):
    reads(f"fdual_sec_mov_{msrc}", f"fdual.fmad.mov r3, r0, r1, r2  [skipinv nosched sd=i0 msrc={msrc}]", ("i0",))
# sd = i0s0 (the first source from the src0 slot) and the src1 / src2 slots as sb / sc
for sec in ("fmad", "fadd", "fmul", "fexp2"):
    reads(f"fdual_i0s0_{sec}", f"fdual.mov.{sec} r3, r0, r1, r2  [skipinv nosched sd=i0s0 sb=i1 sc=i2]", ("i0",))
for sb, sc in (("src1", "i2"), ("i1", "src2"), ("src1", "src2")):
    reads(f"fdual_fmad_{sb}_{sc}", f"fdual.mov.fmad r3, r0, r1, r2  [skipinv nosched sd=i0 sb={sb} sc={sc}]", ("i0",))
# the negates of the src1 / src2 slots: seen by the primary, the secondary, or both
reads("fdual_neg", "fdual.fmad.fmad r3, r0, -r1, -r2  [skipinv nosched sd=i0 sb=src1 sc=src2]", ("r3", "i0"))

# --- fdual: the primary op ------------------------------------------------------------------
# beside a secondary that changes nothing (i0 = i0)
for pri in ("fmad", "mov", "fadm", "fmsa", "fddp", "fminmax", "ima32", "fssq"):
    reads(f"fdual_pri_{pri}", f"fdual.{pri}.mov r3, r0, r1, r2  [skipinv nosched sd=i0 msrc=i0]", ("r3",))
# fddp's second result: the internal registers after it
reads("fdual_fddp", "fdual.fddp.mov r3, r0, r1, r2  [skipinv nosched sd=i0 msrc=i0]", IR)

# --- fdual: pairing and timing --------------------------------------------------------------
# both ops reading the same slots (sb = src1, sc = src2) and both writing
reads("fdual_both", "fdual.fmad.fmad r3, r0, r1, r2  [skipinv nosched sd=i0 sb=src1 sc=src2]", ("r3", "i0"))
# the primary reading the internal register the secondary writes in the same word (r124 = i0)
reads("fdual_pri_reads_sd", "fdual.mov.fexp2 r3, r0, r124, r2  [skipinv nosched sd=i0 sb=i1 sc=i2]", ("r3", "i0"))
# the secondary reading an internal register the primary's destination names (r125 = i1)
reads("fdual_dst_i1", "fdual.fmad.mov r125, r0, r1, r2  [skipinv nosched sd=i0 msrc=i1]", ("i0", "i1"))
# a repeat of 2: both halves step?
SRC2 = {"r8": F(2.0), "r9": F(4.0), "r10": F(3.0), "r11": F(6.0), "r12": F(5.0), "r13": F(10.0), "r14": 0x5a5a5a5a}
for r in ("r14", "r15", "i0", "i1", "i2"):
    TESTS.append((f"fdual_rpt2_{r}", PRE + load(SRC2) + [
        "fdual.fmad.fadd r14, r8, r10, r12  [skipinv nosched rpt=2 sd=i0 sb=i1 sc=i2]",
        f"mov r3, {r}  {M1}", out("r3")], None))
