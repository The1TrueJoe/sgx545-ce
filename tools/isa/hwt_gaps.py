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

# i0 across a deschedule (four loads and a wdf), without and with nosched on
# the instructions that have it (ld has no nosched bit); and with no load at
# all, the control
for ns in (False, True):
    f = " nosched" if ns else ""
    name = "i0_deschedule" + ("_nosched" if ns else "")
    BUFS[name] = BYTES
    TESTS.append((name, [
        "limm r2, #0x12345678  [skipinv]",
        f"limm r1, BUF  [skipinv{f}]",
        f"mov i0, r2  [skipinv{f}]",
        "ld.a.d r3, r1, #0  [skipinv]",
        "ld.a.d r4, r1, #1  [skipinv]",
        "ld.a.d r5, r1, #2  [skipinv]",
        "ld.a.d r6, r1, #3  [skipinv]",
        "wdf",
        "mov r7, i0  [skipinv]",
        f"mov o0, r7  {END}"], None))
TESTS.append(("i0_no_load", [
    "limm r2, #0x12345678  [skipinv]",
    "mov i0, r2  [skipinv]",
    "mov r3, r2  [skipinv]",
    "mov r4, r2  [skipinv]",
    "mov r7, i0  [skipinv]",
    f"mov o0, r7  {END}"], 0x12345678))
