"""hwt suite for hwt_open.py: which part of hwt_mrt's two-target end-of-tile
program hangs the GPU? (docs/GL3.md, H1 results.)

`eot_twice_k0_f0` hung the GPU on a CE5300 board (job timeout, reset).
These split it up, each to be run on its own with -k, since one hang stops
sgx_hwt:

- `eot_rt2_only`: RT2's PBE words alone, ended. RT keeps the clear colour,
  and RT2 reads 0x11111111 if the second target is set up right.
- `eot_rt_twice`: the frame's target emitted twice, the first emit not
  ending the program. Does a non-final emit hang by itself?
- `eot_twice_phas`: both targets with a `phas` wait between the two emits.

Can hang the GPU: run in a slot where a lockup is acceptable.
"""
from hwt_mrt import FS, PHAS, SIDE, target

TESTS = [
    ("eot_rt2_only", FS, None),
    ("eot_rt_twice", FS, 0x11111111),
    ("eot_twice_phas", FS, 0x11111111),
]
EOTS = {
    "eot_rt2_only": [PHAS] + target("RT2", SIDE, True),
    "eot_rt_twice": [PHAS] + target("RT", SIDE, False) + target("RT", SIDE, True),
    "eot_twice_phas": [PHAS] + target("RT", SIDE, False) + [PHAS] + target("RT2", SIDE, True),
}
