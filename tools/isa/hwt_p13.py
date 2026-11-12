"""hwt.py suite: pck.u8.u8 byte picks, which the compiler uses to write a texel to
the colour register in BGRA order (ISA.md "Pack/unpack": an 8-bit source's lane
picks a byte; the first source fills the lowest dmask lane, the second the next).
r0 = 0x44332211: byte n is 0x11 * (n + 1), so every result byte names its source."""
from hwt_lib import load, out

TESTS = []
SRC = {"r0": 0x44332211, "r3": 0x5a5a5a5a}


def lane(n):
    """r0's byte n (lane 0 is written without .l0)"""
    return f"r0.l{n}" if n else "r0"


# one pick per lane pair: each result byte from the lane its source names
for dm, (a, b) in ((3, (2, 1)), (12, (0, 3)), (6, (3, 0)), (9, (1, 2))):
    lo = [k for k in range(4) if dm >> k & 1]
    want = 0x5a5a5a5a
    for k, src in zip(lo, (a, b)):
        want = (want & ~(0xff << 8 * k)) | (0x11 * (src + 1)) << 8 * k
    TESTS.append((f"pck_u8u8_dm{dm}_l{a}{b}", load(SRC) + [
        f"pck.u8.u8 r3, {lane(a)}, {lane(b)}  [skipinv dmask={dm}]", out("r3")], want))

# the compiler's sequence: RGBA8 in r0 -> BGRA8 in r3 (B, G then R, A)
TESTS.append(("pck_u8u8_rgba_to_bgra", load(SRC) + [
    "pck.u8.u8 r3, r0.l2, r0.l1  [skipinv dmask=3]",
    "pck.u8.u8 r3, r0, r0.l3  [skipinv dmask=12]",
    out("r3")], 0x44112233))

# test masks as booleans: or / and of 0 and 0xffffffff, and test.and.eq into a
# predicate (an if on a mask), each result read back
MASKS = {"r0": 0xffffffff, "r1": 0x00000000, "r2": 0xffffffff, "r3": 0x5a5a5a5a}
for op, a, b, want in (("or", "r0", "r1", 0xffffffff), ("or", "r1", "r1", 0),
                       ("and", "r0", "r2", 0xffffffff), ("and", "r0", "r1", 0)):
    TESTS.append((f"mask_{op}_{a}{b}", load(MASKS) + [f"{op} r3, {a}, {b}  [skipinv]", out("r3")], want))
for m, want in (("r0", 0x22222222), ("r1", 0x11111111)):
    TESTS.append((f"mask_test_and_eq_{m}", load(MASKS) + [
        "limm r3, #0x22222222  [skipinv]",
        f"test.and.eq p0, {m}, {m}  [skipinv]",
        "(p0) limm r3, #0x11111111  [skipinv]",
        out("r3")], want))
