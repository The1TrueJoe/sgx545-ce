"""hwt.py suite: test's chan field. These lines ran with the assembler of the time, which dropped
.c10/.f16 on test when [fmt] wasn't given, so they are fpsub8 / fsub on plain 32-bit registers
(written that way below, so they assemble to the words drawn). The format-select versions are hwt_p10."""
from hwt_lib import H, load, out

TESTS = []
M1 = "[skipinv mask=1]"
R1 = 0x020 | 0x020 << 10 | 0x020 << 20
for pat in ("TTT", "TTF", "FTT", "TFT", "FFF", "FTF", "TFF", "FFT"):
    r0 = sum((0x010 if p == "T" else 0x050) << 10 * c for c, p in enumerate(pat))   # T: channel c below 0x020
    for ch in range(8):
        TESTS.append((f"chan_{pat}_{ch}", load({"r0": r0, "r1": R1, "r3": 0xaaaaaaaa})
                      + [f"test.fpsub8.lt p0, r0, r1  [skipinv chan={ch}]", f"(p0) mov r3, c52  {M1}", out("r3")], None))
# F16 compare: r0 = (3, 10), r1 = (5, 1): the low lanes compare true, the high lanes false
for ch in range(8):
    TESTS.append((f"chan_f16_{ch}", load({"r0": H(3, 10), "r1": H(5, 1), "r3": 0xaaaaaaaa})
                  + [f"test.fsub.lt p0, r0, r1  [skipinv chan={ch}]", f"(p0) mov r3, c52  {M1}", out("r3")], None))
# and an f32 compare, for reference (chan 0 is what the compiler uses)
for ch in range(8):
    TESTS.append((f"chan_f32_{ch}", load({"r0": 0x3f800000, "r1": 0x40000000, "r3": 0xaaaaaaaa})
                  + [f"test.fsub.lt p0, r0, r1  [skipinv chan={ch}]", f"(p0) mov r3, c52  {M1}", out("r3")], None))
