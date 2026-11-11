"""hwt suite for the ISA.md gaps (run with hwt_open.py: the open stack, a data
buffer): load sizes, and internal registers across a deschedule.

ld: the buffer holds bytes 0x00, 0x01, ... in order (dword k = 4k+3 .. 4k), so
a result names the bytes it got. ld.a.d #i reads base + 4·(i + 1) (ISA.md
"hw 1d: ld"); what #i means for .w and .b is the gap.

Internal registers: ISA.md "fdual for a compiler" says i0-i3 don't survive a
deschedule, and the compiler marks nosched to keep them. Here i0 is written,
the task waits on loads, and i0 is read back, with and without nosched.
"""
from hwt_lib import END

TESTS = []
BUFS = {}

BYTES = [(4 * k + 3) << 24 | (4 * k + 2) << 16 | (4 * k + 1) << 8 | 4 * k for k in range(16)]

# the readout path through the winsys
TESTS += [
    ("sanity_limm", ["limm r0, #0x12345678  [skipinv]", f"mov o0, r0  {END}"], 0x12345678),
    ("sanity_c52", [f"mov o0, c52  {END}"], 0x3f800000),
]

# ld sizes: d confirms the known offset rule, w and b are the gap
LD = [("d", 0, BYTES[1]), ("d", 2, BYTES[3])] + [("w", i, None) for i in range(4)] + [("b", i, None) for i in range(6)]
for size, off, want in LD:
    name = f"ld_{size}_{off}"
    BUFS[name] = BYTES
    TESTS.append((name, [
        "limm r0, #0x5a5a5a5a  [skipinv]",
        "limm r1, BUF  [skipinv]",
        f"ld.a.{size} r0, r1, #{off}  [skipinv]",
        "wdf",
        f"mov o0, r0  {END}"], want))

# i0 across a deschedule, as hwt_p12 sets internal registers (mov i0 with
# mask=1, nosched from the write to the read). The control keeps nosched on
# every instruction; then the same without nosched; then loads and a wdf in
# between (ld has no nosched bit, wdf keeps it)
NS = "[skipinv nosched mask=1]"
M1 = "[skipinv mask=1]"
I0 = ["limm r2, #0x12345678  [skipinv]", "limm r1, BUF  [skipinv]"]
TESTS.append(("i0_control", I0 + [f"mov i0, r2  {NS}", f"mov r3, r2  {NS}", f"mov r4, r2  {NS}",
                                  f"mov r7, i0  {M1}", f"mov o0, r7  {END}"], 0x12345678))
TESTS.append(("i0_no_nosched", I0 + [f"mov i0, r2  {M1}", f"mov r3, r2  {M1}", f"mov r4, r2  {M1}",
                                     f"mov r7, i0  {M1}", f"mov o0, r7  {END}"], None))
for nld in (1, 4):
    name = f"i0_across_{nld}ld_wdf"
    BUFS[name] = BYTES
    TESTS.append((name, I0 + [f"mov i0, r2  {NS}"] +
                  [f"ld.a.d r{3 + k}, r1, #{k}  [skipinv]" for k in range(nld)] +
                  ["wdf  [nosched]", f"mov r7, i0  {M1}", f"mov o0, r7  {END}"], None))

# test with a destination (l20): does it write the ALU result? r0 - r1 = -1.0
F2, F3 = 0x40000000, 0x40400000
TESTS.append(("test_dst_alu_result", [
    f"limm r0, #{F2:#x}  [skipinv]", f"limm r1, #{F3:#x}  [skipinv]", "limm r2, #0x5a5a5a5a  [skipinv]",
    "test.fsub.lt p0, r2, r0, r1  [skipinv]", f"mov o0, r2  {END}"], None))

# test [mask=15]: one predicate per channel, p0-p3? r0-r3 = 1, 5, 1, 5 against r4-r7 = 3: lt on 0, 2
ONE, FIVE, THREE = 0x3f800000, 0x40a00000, 0x40400000
for p in range(4):
    TESTS.append((f"test_mask15_p{p}", [
        f"limm r0, #{ONE:#x}  [skipinv]", f"limm r1, #{FIVE:#x}  [skipinv]",
        f"limm r2, #{ONE:#x}  [skipinv]", f"limm r3, #{FIVE:#x}  [skipinv]"] +
        [f"limm r{4 + k}, #{THREE:#x}  [skipinv]" for k in range(4)] + [
        "limm r8, #0x22222222  [skipinv]",
        "test.fsub.lt p0, r0, r4  [skipinv mask=15]",
        f"(p{p}) limm r8, #0x11111111  [skipinv]", f"mov o0, r8  {END}"], None))

# fddp: the second result into i1 (h19)
for r in ("r3", "i1"):
    TESTS.append((f"fddp_{r}", [
        f"limm r0, #{ONE:#x}  [skipinv]", f"limm r1, #{F2:#x}  [skipinv]", f"limm r2, #{F3:#x}  [skipinv]",
        "limm r3, #0x5a5a5a5a  [skipinv]", f"mov i1, r3  {NS}",
        f"fddp r3, i1, r0, r1, r2  [skipinv nosched]", f"mov r7, {r}  {M1}", f"mov o0, r7  {END}"], None))

# ima32 [hi=i1]: the product's high 32 bits into i1? 0x10000 * 0x10000 = 2^32
for r in ("r3", "i1"):
    TESTS.append((f"ima32_hi_{r}", [
        "limm r0, #0x10000  [skipinv]", "limm r1, #0x10000  [skipinv]", "limm r2, #0  [skipinv]",
        "limm r4, #0x5a5a5a5a  [skipinv]", f"mov i1, r4  {NS}",
        "ima32 r3, r0, r1, r2  [skipinv nosched hi=i1]", f"mov r7, {r}  {M1}", f"mov o0, r7  {END}"], None))
