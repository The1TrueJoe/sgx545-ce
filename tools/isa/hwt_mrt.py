"""hwt suite for hwt_open.py: can a pixel carry a second output to the PBE?
(docs/GL3.md, test H1: MRT and the formats wider than 32 bits.)

The fragment program writes o1 = 0x22222222, then o0 = 0x11111111 and ends.
The end-of-tile program is the test's (EOTS): the frame's own program
(STATE.md §4: four LIMMs of the PBE words, an emit of them, an emit of the
sideband word) for the target RT, then the same for RT2, a second 720x480
BGRA surface, with the sideband's b6-11 ("a per-surface value added for
format codes 3-14", 0 in every capture) set to k. If RT2's middle pixel
reads 0x22222222 for some k, b6-11 selects the output register.

- `eot_own`: the frame's single-target program, written by the test: RT
  must read 0x11111111, as without EOTS. It checks the hook.
- `eot_twice_k<k>_f<f>`: both targets, RT2's sideband format f (0 BGRA8888,
  1 RGBA8888, 3 RGB565: codes 3-14 are the ones the add is said to apply to)
  and b6-11 = k. RT2 reads 0 if the PBE wrote nothing there.

A 2-target end-of-tile program and an o1 write are unknown territory: run in
a slot where a GPU lockup is acceptable.
"""
from hwt_lib import END

TESTS = []
EOTS = {}

# the PBE words of the 720x480 target, pitch 736 (STATE.md §4, ref.bin)
W0, W1, W2 = 0x00000000, 0x01DF02CF, 0x016F8000
SIDE = 0x600  # format 0 (BGRA8888), b9, b10 dither


def side_emit(side, end):
    lo = (side & 0x3F) << 22 | 0x00208180
    hi = 0xFB200000 | (0x40000 if end else 0) | (side & 0x3000) << 10 | ((side >> 3) & 0x1F8)
    return f"raw {hi:08x} {lo:08x}"


def target(addr, side, end):
    return [
        f"limm r0, #{W0:#010x}",
        f"limm r1, #{W1:#010x}",
        f"limm r2, #{W2:#010x}",
        f"limm r3, {addr}",
        "raw fb200100 00200080",  # emit the four PBE words
        side_emit(side, end),
    ]


PHAS = "raw fa440700 00000000"  # phas.i #0, #0 [wait=7]

FS = [
    "limm r4, #0x11111111  [skipinv]",
    "limm r5, #0x22222222  [skipinv]",
    "mov o1, r5  [skipinv]",
    f"mov o0, r4  {END}",
]

TESTS.append(("eot_own", FS, 0x11111111))
EOTS["eot_own"] = [PHAS] + target("RT", SIDE, True)

for f in (0, 1, 3):
    for k in (0, 1, 2, 4, 8, 16, 32):
        name = f"eot_twice_k{k}_f{f}"
        TESTS.append((name, FS, 0x11111111))
        EOTS[name] = [PHAS] + target("RT", SIDE, False) + \
            target("RT2", (SIDE & ~0x3F) | f | k << 6, True)
