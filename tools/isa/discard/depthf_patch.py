"""Patch libGLESv2.so.1.7.862890 so the points/lines discard path writes its
(p1) depthf into the fragment program (DDK 1.7 stores it into stack slots).

At 0x37304 (file vaddr) the driver has 33 bytes that compute
lo = (n + 2) << 14 | 0xf0000184 into 0xb8(%esp) (the slot pointer) and
hi = 0xfb340404 into 0xbc(%esp) (its temp-count variable). Replace them with
the same computation stored through the slot pointer; ecx is preserved.
usage: depthf_patch.py IN OUT
"""
import hashlib, struct, sys

VA = 0x37304
OLD = bytes.fromhex("8b442474" "c78424bc00000004 0434fb".replace(" ", "") +
                    "83c002" "c1e00e" "0d840100f0" "898424b8000000")
NEW = bytes.fromhex(
    "8b442474"          # mov 0x74(%esp),%eax        n
    "83c002"            # add $2,%eax
    "c1e00e"            # shl $14,%eax
    "0d840100f0"        # or $0xf0000184,%eax        lo
    "51"                # push %ecx
    "8b8c24bc000000"    # mov 0xbc(%esp),%ecx        slot pointer (0xb8 + 4)
    "8901"              # mov %eax,(%ecx)
    "c7410404 0434fb".replace(" ", "")  # movl $0xfb340404,4(%ecx)  hi
    + "59")             # pop %ecx
assert len(OLD) == len(NEW) == 33, (len(OLD), len(NEW))

d = bytearray(open(sys.argv[1], "rb").read())
assert hashlib.sha1(d).hexdigest() == "6a4dd0ce98a8639a8ae76a671fd048954f7913e4", "not the DDK 1.7 libGLESv2"
ph, n = struct.unpack_from("<I", d, 0x1c)[0], struct.unpack_from("<H", d, 0x2c)[0]
for i in range(n):
    t, o, v, _, fs, _, _, _ = struct.unpack_from("<8I", d, ph + 32 * i)
    if t == 1 and v <= VA < v + fs:
        off = VA - v + o
        break
assert d[off:off + 33] == OLD, d[off:off + 33].hex()
d[off:off + 33] = NEW
open(sys.argv[2], "wb").write(d)
print("patched at file offset %#x" % off)
