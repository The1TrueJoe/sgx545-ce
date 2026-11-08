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
