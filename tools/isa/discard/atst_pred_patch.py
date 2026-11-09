"""Write variants of libGLESv2.so.1.7.862890 whose atst8 (the discard feedback,
hi word 0xf9340406 at 0x3767b) has h9-10 = 0 (none), 1 (p0) or 3 (!p0)
instead of the driver's 2 (p1). usage: atst_pred_patch.py IN OUT 00|02|06"""
import sys

OFF = 0x3767b  # movl $0xf9340406, 0x4(%eax); .text file offset == vaddr
d = bytearray(open(sys.argv[1], "rb").read())
assert d[OFF:OFF + 7] == bytes.fromhex("c74004060434f9"), d[OFF:OFF + 7].hex()
d[OFF + 4] = int(sys.argv[3], 16)
open(sys.argv[2], "wb").write(d)
