#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Before loading modules on a board, check their symbol versions against the
# board's own modules (32-bit little-endian ELF, MODVERSIONS):
#
#   modver-check.py <dir of the board's .ko files> <module.ko>...
#
# Every symbol a module imports that one of the board's modules imports too
# must carry the same CRC; a different CRC means the kernel refuses the module
# ("disagrees about version of symbol"). Exits 1 if any differs.
import glob
import os
import struct
import sys


def versions(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        sys.exit(f"{path}: not a 32-bit ELF module")
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2e)
    secs = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = secs[shstrndx][4]
    out = {}
    for s in secs:
        name = d[strtab + s[0]:d.index(b"\0", strtab + s[0])]
        if name != b"__versions":
            continue
        # struct modversion_info: a 4-byte CRC, then the name, 64 bytes in all
        for off in range(s[4], s[4] + s[5], 64):
            crc = struct.unpack_from("<I", d, off)[0]
            out[d[off + 4:off + 64].split(b"\0")[0].decode()] = crc
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: modver-check.py BOARD_MODULE_DIR MODULE.ko...")
    board = {}
    for p in glob.glob(os.path.join(sys.argv[1], "**", "*.ko"), recursive=True):
        board.update(versions(p))
    bad = 0
    for p in sys.argv[2:]:
        v = versions(p)
        diff = sorted(k for k, c in v.items() if k in board and board[k] != c)
        same = sum(1 for k, c in v.items() if board.get(k) == c)
        print(f"{os.path.basename(p)}: {len(v)} imports, {same} as on the board, "
              f"{len(diff)} differ{': ' + ' '.join(diff[:8]) if diff else ''}")
        bad += len(diff)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
