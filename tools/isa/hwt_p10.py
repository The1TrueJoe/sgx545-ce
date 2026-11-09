"""hwt.py suite: test with real format select (fmt, h18). p7-p9's `test ... .f16/.c10` lines had no
[fmt], and the assembler of the time silently dropped the suffix, so those draws compared f32 words
(their logs stay as they are). Which half / register an F16 test operand reads, and test chan."""
from hwt_lib import H, load, out

TESTS = []
M1 = "[skipinv mask=1]"
# r0 = (3, 10), r1 = (5, 1), r2 = (7, 0.5). Number n read as: half index -> 3, 10, 5;
# register low half -> 3, 5, 7; register high half -> 10, 1, 0.5.
R = {"r0": H(3, 10), "r1": H(5, 1), "r2": H(7, 0.5), "r3": 0xaaaaaaaa}
h = lambda n: f"r{n >> 1}.f16.{('lo', 'hi')[n & 1]}"     # encoded number n (it counts halves: the answer)
for a, b in ((1, 0), (2, 1)):
    for ch in range(4):
        TESTS.append((f"testfmt_f16_n{a}_lt_n{b}_chan{ch}", load(R)
                      + [f"test.fsub.lt p0, {h(a)}, {h(b)}  [skipinv fmt chan={ch}]", f"(p0) mov r3, c52  {M1}", out("r3")], None))
# C10 compare with fmt: channel c of r0 below r1's 0x020 where the pattern says T
R1 = 0x020 | 0x020 << 10 | 0x020 << 20
for pat in ("TFF", "FTF", "FFT", "TTT", "FFF"):
    r0 = sum((0x010 if p == "T" else 0x050) << 10 * c for c, p in enumerate(pat))
    for ch in range(8):
        TESTS.append((f"testfmt_c10_{pat}_chan{ch}", load({"r0": r0, "r1": R1, "r3": 0xaaaaaaaa})
                      + [f"test.fpsub8.lt p0, r0.c10, r1.c10  [skipinv fmt chan={ch}]", f"(p0) mov r3, c52  {M1}", out("r3")], None))
