"""Compare sgxtex_test's output (stdin) with tex.py and present.py."""
import sys

import present
import tex

bad = n = 0
blit = None
words = []
for line in sys.stdin:
    t = line.split()
    if t[0] == "tex":
        w, h, bpp, lv = map(int, t[1:5])
        lw, lh = tex.level_size(w, h, lv)
        want = [tex.texel_offset(x, y, w, h, bpp, lv) for y in range(lh) for x in range(lw)]
        got = list(map(int, t[5:]))
        n += len(want)
        bad += sum(a != b for a, b in zip(want, got)) + abs(len(want) - len(got))
    elif t[0] == "blit":
        src, w, h, sp, dst, dp = (int(x, 0) for x in t[1:])
        blit = dict(present.blit_pokes(src, w, h, sp, dst, dp))
    elif t[0] == "words":
        va = int(t[1], 0)
        got = [int(x, 0) for x in t[2:]]
        n += len(got)
        bad += sum(a != b for a, b in zip(blit[va], got)) + abs(len(blit[va]) - len(got))
print(f"sgxtex.c vs tex.py/present.py: {n - bad} of {n} values agree, {bad} differ")
sys.exit(1 if bad else 0)
