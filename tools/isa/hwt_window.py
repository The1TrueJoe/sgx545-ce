"""hwt.py suite: USE code-base windows. Which window a DOUTU code base runs, and where
`ba`'s absolute target counts from when the task's base isn't 0.

One extra pixel-heap buffer (0x37016000, 0x110000 bytes) puts code in windows 1 and 2
(0x37080000, 0x37100000). Each window w gets, at the same offset as the main program:
  w + 0x8380  phas.i #0, #0 [wait=7], then an entry body: o0 = 0xe0e0000w (or a ba)
  w + 0x8480  a landing program: o0 = 0x5a5a000w
DOUTU word 0 (0x2e018020) names base b, offset 0x8380. Every place a jump can reach holds
a program that ends, so a wrong guess shows as a marker instead of running on.
"""
import sys

import hwt_lib

MAIN = sys.modules["__main__"]
WIN = 0x80000
ENTRY, LAND = 0x8380, 0x8480
HEAP = 0x37000000
EXTRA = "alloc 196 4 0x1 0x110000 0x1000 0x37016000\nmap 196\n"
DOUTU0, DOUTU0_W = 0x2e018020, 0x04010700


def _base(work, _orig=MAIN.base):
    tmpl, data = _orig(work)
    i = tmpl.index("map 195\n") + len("map 195\n")
    return tmpl[:i] + EXTRA + tmpl[i:], data


MAIN.base = _base


def words(lines):
    return MAIN.program(lines)


def marker(v):
    return words(hwt_lib.load({"r4": v}) + [hwt_lib.out("r4")])


def E(w):
    return 0xe0e00000 | w


def M(w):
    return 0x5a5a0000 | w


def layout(base, bodies=None):
    """pokes: every window's header, entry body and landing; DOUTU at base"""
    bodies = bodies or {}
    p = [(DOUTU0, [DOUTU0_W | base])]
    for w in range(3):
        va = HEAP + w * WIN
        p.append((va + ENTRY, [0x00000000, 0xfa440700]))
        p.append((va + ENTRY + 8, words(bodies[w]) if w in bodies else marker(E(w))))
        p.append((va + LAND, marker(M(w))))
    return p


T_WIN = LAND // 8                 # 0x1090: the landing, counted from the task's window
T_HEAP1 = (WIN + LAND) // 8       # 0x11090: window 1's landing, counted from the heap

TESTS = [
    # which window's code a DOUTU base runs
    ("base0", [], E(0), layout(0)),
    ("base1", [], E(1), layout(1)),
    ("base2", [], E(2), layout(2)),
    # ba's absolute target: window-relative predicts M(task window), heap-relative M(0) / M(1)
    ("ba_b0_win", [], M(0), layout(0, {0: [f"ba #{T_WIN:#x}  [nosched]"]})),
    ("ba_b1_win", [], M(1), layout(1, {1: [f"ba #{T_WIN:#x}  [nosched]"]})),
    ("ba_b1_heap", [], M(2), layout(1, {1: [f"ba #{T_HEAP1:#x}  [nosched]"]})),
    ("ba_b2_win", [], M(2), layout(2, {2: [f"ba #{T_WIN:#x}  [nosched]"]})),
]
