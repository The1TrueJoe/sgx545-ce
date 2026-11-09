"""hwt.py suite: `ba` (absolute branch). The program's first instruction is at 0x37008388, instruction
0x1071 from code base 0 (0x37000000), which is how DOUTU addresses it; ISA.md: target = label + load offset."""
from hwt_lib import load, out

PADS = load({"r4": 0x44, "r5": 0x55, "r6": 0x66, "r7": 0x77})   # instructions 0-3
BASE = 0x8388 // 8
TESTS = [("ba_abs", PADS + [f"ba #{BASE + 6:#x}  [nosched]"] + [out(f"r{4 + i}") for i in range(4)], 0x55)]
