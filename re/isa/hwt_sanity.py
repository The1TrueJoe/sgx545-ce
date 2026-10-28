"""hwt.py suite: the readout path itself (limm, mov to o0, a known constant)."""
END = "[skipinv end mask=1]"
TESTS = [
    ("limm", ["limm r0, #0x12345678  [skipinv]", f"mov o0, r0  {END}"], 0x12345678),
    ("c52", [f"mov o0, c52  {END}"], 0x3f800000),
    ("limm_r15", ["limm r15, #0xdeadbeef  [skipinv]", f"mov o0, r15  {END}"], 0xdeadbeef),
]
