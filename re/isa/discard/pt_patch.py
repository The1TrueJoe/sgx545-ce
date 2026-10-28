"""Patch libGLESv2.so.1.7.862890 so a discarding shader drawn as points or
lines takes the triangle path (ISPA pass type 4 or 2, atst8 feedback,
ctx+0x7a0 = 1) instead of the depth-only path (pass type 5, ctx+0x7a0 = 9),
which renders a broken frame on the SGX545 (EA3, 2026-10-28).

At 0x39d55 (FUN_00049a00) the driver reads the primitive (ctx+0xc74) and,
for points/lines, switches to pass type 5. Replace the read with
`movl $1, 0x34(%esp); jmp 0x39d90` (the triangle result).
usage: pt_patch.py IN OUT
"""
import hashlib, sys

OFF = 0x39d55  # .text file offset == vaddr in this library
OLD = bytes.fromhex("8b96740c000085d20f8495010000")
NEW = bytes.fromhex("c744243401000000" "eb31" "90909090")
assert len(OLD) == len(NEW)
d = bytearray(open(sys.argv[1], "rb").read())
assert hashlib.sha1(d).hexdigest() == "6a4dd0ce98a8639a8ae76a671fd048954f7913e4", "not the DDK 1.7 libGLESv2"
assert d[OFF:OFF + len(OLD)] == OLD, d[OFF:OFF + len(OLD)].hex()
d[OFF:OFF + len(NEW)] = NEW
open(sys.argv[2], "wb").write(d)
print("patched")
