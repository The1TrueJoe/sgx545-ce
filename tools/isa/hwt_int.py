"""hwt suite: the integer semantics the compiler's native_integers path relies
on (GLSL 1.30 ints, docs/COMPILER-GL3.md). Run with hwt_open.py.

Each test loads its operands with limm, runs one instruction (or the
compiler's sequence for one operation) and moves the result to o0. The
expected words are what the compiler assumes; a test that fails is a fact to
fix in ISA.md and in the compiler. Already hardware in ISA.md, and not
repeated: the bitwise ops with an immediate, shr by a register, a shift of 33
giving 0, testmask.fsub's dword masks, movc's conditions, pck f32 -> s16
rounding, pck.f32.f32 [rint] modes, ima32's high word into i1 (0x10000²).
"""
from hwt_lib import END, F

TESTS = []
NS = "[skipinv nosched mask=1]"
M1 = "[skipinv mask=1]"


def t(name, regs, ins, want, res="r3"):
    lines = [f"limm r{i}, #{v & 0xffffffff:#x}  [skipinv]" for i, v in enumerate(regs)]
    lines.append("limm r3, #0x5a5a5a5a  [skipinv]")
    lines += ins
    lines.append(f"mov o0, {res}  {END}")
    TESTS.append((name, lines, None if want is None else want & 0xffffffff))


# ima32: d = s0 * s1 + s2, low 32 bits; isub as + -s2
t("ima32_add_wrap", [0x7fffffff, 1], ["ima32 r3, r0, #1, r1  [skipinv]"], 0x80000000)
t("ima32_add_carry", [0xffffffff, 2], ["ima32 r3, r0, #1, r1  [skipinv]"], 1)
t("ima32_sub", [5, 7], ["ima32 r3, r0, #1, -r1  [skipinv]"], -2)
t("ima32_sub_min", [0x80000000, 1], ["ima32 r3, r0, #1, -r1  [skipinv]"], 0x7fffffff)
t("ima32_sub_imm", [3], ["ima32 r3, r0, #1, -#7  [skipinv]"], -4)
t("ima32_add_imm", [0xfffffff0], ["ima32 r3, r0, #1, #100  [skipinv]"], 0x54)
t("ima32_mul_lo", [0x12345678, 0x9abcdef], ["ima32 r3, r0, r1, #0  [skipinv]"],
  (0x12345678 * 0x9abcdef))
t("ima32_mul_neg", [-3, 5], ["ima32 r3, r0, r1, #0  [skipinv]"], -15)
t("ima32_mul_imm", [0x01000001], ["ima32 r3, r0, #100, #0  [skipinv]"], 0x64000064)
# -a as a * ~0 (c56): the low word whatever the signedness
t("ima32_neg_c56", [12345], ["ima32 r3, r0, c56, #0  [skipinv]"], -12345)
t("ima32_neg_c56_min", [0x80000000], ["ima32 r3, r0, c56, #0  [skipinv]"], 0x80000000)
# the high word: unsigned, and signed with .sgn
for name, a, b, sgn, want in [
        ("u_ff", 0xffffffff, 0xffffffff, False, 0xfffffffe),
        ("u_big", 0x89abcdef, 0x12345678, False, (0x89abcdef * 0x12345678) >> 32),
        ("s_m1m1", -1, -1, True, 0),
        ("s_m2x3", -2, 3, True, -1),
        ("s_big", 0x89abcdef, 0x12345678, True, ((0x89abcdef - (1 << 32)) * 0x12345678) >> 32)]:
    t(f"ima32_hi_{name}", [a, b], [
        f"ima32{'.sgn' if sgn else ''} r4, r0, r1, #0  [skipinv nosched hi=i1]",
        f"mov r3, i1  {M1}"], want)

# testmask with the bitwise ALU ops: ~0 / 0 dwords
for name, a, b, alu, cond, want in [
        ("xor_eq_t", 7, 7, "xor", "eq", -1), ("xor_eq_f", 7, 6, "xor", "eq", 0),
        ("xor_ne_t", 0x80000000, 0, "xor", "ne", -1), ("xor_ne_f", -1, -1, "xor", "ne", 0),
        ("or_neg_t", 0x80000000, 0, "or", "neg", -1), ("or_neg_f", 0x7fffffff, 0, "or", "neg", 0),
        ("or_neg_or", 0, 0x80000000, "or", "neg", -1),
        ("or_pos_t", 0, 0, "or", "pos", -1), ("or_pos_f", 0, -5, "or", "pos", 0),
        ("or_gt_t", 1, 0, "or", "gt", -1), ("or_gt_zero", 0, 0, "or", "gt", 0),
        ("or_gt_neg", -1, 0, "or", "gt", 0),
        ("or_le_zero", 0, 0, "or", "le", -1), ("or_le_neg", -7, 0, "or", "le", -1),
        ("or_le_pos", 7, 0, "or", "le", 0)]:
    t(f"testmask_{name}", [a, b], [f"testmask.{alu}.{cond}.dword r3, r0, r1  [skipinv]"], want)
t("testmask_or_neg_imm0", [-3], ["testmask.or.neg.dword r3, r0, #0  [skipinv]"], -1)
t("testmask_xor_eq_imm", [5], ["testmask.xor.eq.dword r3, r0, #5  [skipinv]"], -1)

# test with xor / and into p0 (an if's condition), read with a predicated limm
for name, a, b, alu, cond, want in [
        ("xor_ne_t", 3, 4, "xor", "ne", 1), ("xor_ne_f", 3, 3, "xor", "ne", 0),
        ("and_eq_mask", -1, -1, "and", "eq", 0), ("and_eq_zero", 0, 0, "and", "eq", 1)]:
    t(f"test_{name}", [a, b], [
        f"test.{alu}.{cond} p0, r0, r1  [skipinv]",
        "limm r3, #0  [skipinv]", "(p0) limm r3, #1  [skipinv]"], want)

# bitwise: a register src2 inverted, ~a, b2f's mask, asr by a register
t("and_inv_reg", [0xf0f0ff00, 0x0000ffff], ["and.inv r3, r0, r1  [skipinv]"], 0xf0f00000)
t("xor_inv_0", [0x12345678], ["xor.inv r3, r0, #0  [skipinv]"], ~0x12345678)
t("and_b2f", [-1], ["and r3, r0, #0x3f80  [skipinv rot=16]"], 0x3f800000)
t("asr_reg", [0x80000010, 4], ["asr r3, r0, r1  [skipinv]"], 0xf8000001)
t("shl_reg", [0x00000011, 31], ["shl r3, r0, r1  [skipinv]"], 0x80000000)

# int <-> f32 through 16-bit halves
t("pck_f32_s16_hi", [0xfffe0003], ["pck.f32.s16 r3, r0.l2, #0  [skipinv]"], F(-2.0))
t("pck_f32_s16_lo", [0x0003fffe], ["pck.f32.s16 r3, r0, #0  [skipinv]"], F(-2.0))
t("pck_f32_u16_lo", [0xfffe0003], ["pck.f32.u16 r3, r0, #0  [skipinv]"], F(3.0))
t("pck_f32_u16_hi", [0xfffe0003], ["pck.f32.u16 r3, r0.l2, #0  [skipinv]"], F(65534.0))
t("pck_u16_f32_pair", [F(3.0), F(65535.0)], ["pck.u16.f32 r3, r0, r1  [skipinv]"], 0xffff0003)
t("pck_u16_f32_zero_hi", [F(40000.0)], ["pck.u16.f32 r3, r0, #0  [skipinv]"], 40000)
t("pck_u16_f32_clamp", [F(-5.0), F(70000.0)], ["pck.u16.f32 r3, r0, r1  [skipinv]"], 0xffff0000)
# the compiler's i2f: hi * 65536 + lo, one rounding
t("i2f_seq", [-123456789], [
    "pck.f32.s16 r4, r0.l2, #0  [skipinv]", "pck.f32.u16 r5, r0, #0  [skipinv]",
    "limm r6, #0x47800000  [skipinv]", "fmad r3, r4, r6, r5  [skipinv]"], F(-123456789.0))
# frcp accuracy (nir_lower_idiv refines a reciprocal of the divisor):
# recorded, compared with the correctly rounded value offline
for x in (3.0, 7.0, 1000003.0, 4294967040.0, 1.5):
    t(f"frcp_{int(x)}", [F(x)], ["frcp r3, r0  [skipinv mask=1]"], None)
