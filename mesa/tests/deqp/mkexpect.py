#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
"""Mesa CI expectation files from drive-deqp.py results.

    mkexpect.py PREFIX RESULTS.csv [MORE.csv ...] [--wedges WEDGES.txt] [--note TEXT]

Later files override earlier ones test by test (a rerun of the fails after the
first pass). Writes PREFIX-fails.txt (Fail, Crash, Timeout, with the status
deqp-runner compares against), PREFIX-flakes.txt (tests that came out Flake)
and PREFIX-skips.txt (tests that wedged the GPU), each with a header saying
where it came from, and prints the totals.
"""
import argparse
import collections
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("results", nargs="+")
    ap.add_argument("--wedges", default=None)
    ap.add_argument("--note", default="")
    a = ap.parse_args()

    status = {}
    for path in a.results:
        with open(path) as f:
            for line in f:
                parts = line.strip().split(",")
                if len(parts) >= 2 and parts[0].startswith("dEQP"):
                    status[parts[0]] = parts[1]
    wedges = []
    if a.wedges:
        with open(a.wedges) as f:
            wedges = sorted({t.strip() for t in f if t.strip()})

    counts = collections.Counter(status.values())
    head = (f"# {time.strftime('%Y-%m-%d')}: {len(status)} tests, "
            + ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
            + (f"\n# {a.note}" if a.note else "") + "\n")
    fails = sorted(t for t, s in status.items() if s in ("Fail", "Crash", "Timeout"))
    flakes = sorted(t for t, s in status.items() if s == "Flake")
    with open(a.prefix + "-fails.txt", "w") as f:
        f.write(head + "".join(f"{t},{status[t]}\n" for t in fails))
    with open(a.prefix + "-flakes.txt", "w") as f:
        f.write(head + "".join(f"{t}\n" for t in flakes))
    with open(a.prefix + "-skips.txt", "w") as f:
        f.write(head + "# tests that wedged the GPU (drive-deqp.py)\n" + "".join(f"{t}\n" for t in wedges))
    print(head.strip())
    print(f"{len(fails)} fails, {len(flakes)} flakes, {len(wedges)} skips")


if __name__ == "__main__":
    main()
