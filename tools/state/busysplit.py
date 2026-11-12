# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Per-unit busy/idle split of a regsample CSV (tools/ddk/regsample.c), from CLKGATESTATUS.

    python3 tools/state/busysplit.py CSV[.gz]... [--dur-ms 30000] [--idle]

For each capture it prints:
- the share of wall time with the TA and the 3D clocked together, the TA alone,
  the 3D alone, only other units, and nothing at all;
- each unit's total busy share (TA busy = together + alone, likewise the 3D);
- the per-frame TA and 3D segments: the median length of TA segments over 8 ms
  and 3D segments over 5 ms, and how many there were per second;
- busy ms in each second of the window, to show whether the window is steady.
--idle adds a histogram of the all-idle stretches by the unit busy before and
after each one.

The unit groups are SGX545's, read off CLKGATESTATUS (docs/porting/state.md,
"Measuring busy and idle time"). Another core needs its own groups.
"""
import argparse
import collections
import gzip
import statistics

GROUPS = {"3d": 0x3, "ta": 0x58, "shared": 0xCEF80, "pixel": 0x33004}


def load(path):
    f = gzip.open(path, "rt") if path.endswith(".gz") else open(path)
    rows, col = [], None
    with f:
        for line in f:
            if line[0] == "#":
                continue
            p = line.rstrip().split(",")
            if col is None:
                col = p.index("CLKGATESTATUS")
                continue
            rows.append((int(p[0]), int(p[col], 16)))
    return rows


def segments(rows, mask, tpm, gap_ms=0.5):
    """(start, length ms) of the stretches where any bit of mask is set, merging gaps under gap_ms."""
    out, start, last = [], None, None
    for i in range(1, len(rows)):
        t, cs = rows[i - 1]
        if cs & mask:
            if start is None:
                start = t
            last = rows[i][0]
        elif start is not None and (rows[i][0] - last) / tpm > gap_ms:
            out.append((start, (last - start) / tpm))
            start = None
    return out


def kind(cs):
    a, b = cs & GROUPS["3d"], cs & GROUPS["ta"]
    if a and b:
        return "TA+3D"
    if b:
        return "TA"
    if a:
        return "3D"
    return "other" if cs & (GROUPS["shared"] | GROUPS["pixel"]) else "idle"


def report(path, dur_ms, idle_hist):
    rows = load(path)
    tpm = (rows[-1][0] - rows[0][0]) / dur_ms
    share = collections.Counter()
    per_sec = [0.0] * (int(dur_ms // 1000) + 1)
    t0 = rows[0][0]
    for i in range(1, len(rows)):
        (t, cs), dt = rows[i - 1], rows[i][0] - rows[i - 1][0]
        k = kind(cs)
        share[k] += dt
        if k != "idle":
            per_sec[min(int((t - t0) / tpm / 1000), len(per_sec) - 1)] += dt / tpm
    tot = sum(share.values())
    pc = {k: 100 * share[k] / tot for k in ("TA+3D", "TA", "3D", "other", "idle")}
    ta = [d for _, d in segments(rows, GROUPS["ta"], tpm) if d > 8]
    d3 = [d for _, d in segments(rows, GROUPS["3d"], tpm) if d > 5]
    secs = dur_ms / 1000
    print(f"== {path}")
    print(f"  TA+3D {pc['TA+3D']:5.1f}%  TA alone {pc['TA']:5.1f}%  3D alone {pc['3D']:5.1f}%  "
          f"other {pc['other']:4.1f}%  all idle {pc['idle']:5.1f}%")
    print(f"  TA busy {pc['TA+3D'] + pc['TA']:5.1f}%  3D busy {pc['TA+3D'] + pc['3D']:5.1f}%")
    if ta:
        print(f"  TA segments > 8 ms: {len(ta) / secs:.1f}/s, median {statistics.median(ta):.1f} ms")
    if d3:
        print(f"  3D segments > 5 ms: {len(d3) / secs:.1f}/s, median {statistics.median(d3):.1f} ms")
    print("  busy ms per second: " + " ".join(f"{b:.0f}" for b in per_sec[:int(secs)]))
    if idle_hist:
        idle_by_position(rows, tpm, secs)


def idle_by_position(rows, tpm, secs):
    merged = []
    for i in range(1, len(rows)):
        k, d = kind(rows[i - 1][1]), (rows[i][0] - rows[i - 1][0]) / tpm
        if merged and merged[-1][0] == k:
            merged[-1][1] += d
        else:
            merged.append([k, d])

    def unit(j, step):
        while 0 <= j < len(merged):
            if merged[j][0] in ("TA", "3D", "TA+3D"):
                return merged[j][0]
            j += step
        return "?"

    bins = [0.05, 0.2, 0.5, 1, 2, 5, 10, 20, 50, float("inf")]
    by = collections.defaultdict(lambda: [0, 0.0, collections.Counter()])
    for j, (k, d) in enumerate(merged):
        if k == "idle":
            key = f"{unit(j - 1, -1)} -> {unit(j + 1, 1)}"
            by[key][0] += 1
            by[key][1] += d
            by[key][2][next(b for b in bins if d <= b)] += 1
    print("  all-idle stretches by position (before -> after): count, ms per second, length bins (ms)")
    print("    " + " " * 18 + " ".join(f"{'<=' + str(b) if b != float('inf') else '>50':>6s}" for b in bins))
    for key, (n, d, h) in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print(f"    {key:16s} {n:6d} {d / secs:6.1f} " + " ".join(f"{h[b]:6d}" for b in bins))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--dur-ms", type=float, default=30000, help="the regsample duration (its CSV header)")
    ap.add_argument("--idle", action="store_true")
    a = ap.parse_args()
    for path in a.csv:
        report(path, a.dur_ms, a.idle)


if __name__ == "__main__":
    main()
