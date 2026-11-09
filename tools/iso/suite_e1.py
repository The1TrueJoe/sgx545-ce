# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""E1 (docs/DRM-DRIVER.md section 6): does the PTE READONLY bit stop a client task's st?

The DDK maps an allocation whose attributes have READ but not WRITE with the PTE READONLY
bit (0x4). A fragment task stores a sentinel to such a buffer, loads it back, and copies what
it read into a read-write result buffer; the host then dumps both. The control does the same
to a read-write buffer, so a sentinel in it shows the store path itself works.

  RO unchanged on the host     -> the BIF enforced READONLY for this write
  RO shows the sentinel        -> it did not
A fault on the store (kmsg) also means enforced; the rig stops there."""
from iso import T, out

R = ("R", 0, 0xf, 0x1000, 0x1000)


def store_and_read(target):
    return [
        "limm r2, #0xdeadbeef  [skipinv]",
        "st.a.d sa1, #0, r2  [skipinv]",
        "ld.a.d r3, sa1, #0  [skipinv drc=0]",
        "wdf [drc=0]",
        "st.a.d sa0, #0, r3  [skipinv]",
        out("r3"),
    ]


TESTS = [
    T("e1_st_rw_control", bufs=[R, ("B", 0, 0xf, 0x1000, 0x1000)], fills=[("B", 0, [0x11111111])],
      fsa=[(0, "R"), (1, "B")], dumps=[("B", 0, 1), ("R", 0, 1)], fs=store_and_read("B")),
    T("e1_st_readonly", bufs=[R, ("B", 0, 0x9, 0x1000, 0x1000)], fills=[("B", 0, [0x11111111])],
      fsa=[(0, "R"), (1, "B")], dumps=[("B", 0, 1), ("R", 0, 1)], fs=store_and_read("B")),
]
