#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
"""Mesa CI expectation files from drive-deqp.py results.

    mkexpect.py PREFIX RESULTS.csv [MORE.csv ...] [--wedges WEDGES.txt]
                [--reasons REASONS.txt] [--flakes SEEN.txt] [--note TEXT]

Later files override earlier ones test by test (a rerun of the fails after the
first pass). Works for dEQP and piglit results. Writes PREFIX-fails.txt (Fail,
Crash, Timeout, Warn, Missing, with the status the runner compares against), PREFIX-flakes.txt (tests that came out Flake)
and PREFIX-skips.txt (tests that wedged the GPU, or the box), each with a
header saying where it came from, and prints the totals.

REASONS.txt is "REGEX<tab>why" per line: the fails are written in groups under
a "# why" comment, each test under the first regex that matches it, the rest
last under "# not looked at yet".

SEEN.txt names more flakes, one per line: tests seen both passing and failing
on the same build (order-dependent ones that the runner never retried).
"""
import argparse
import collections
import re
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("results", nargs="+")
    ap.add_argument("--wedges", default=None)
    ap.add_argument("--reasons", default=None)
    ap.add_argument("--flakes", default=None)
    ap.add_argument("--note", default="")
    a = ap.parse_args()

    status = {}
    for path in a.results:
        with open(path) as f:
            for line in f:
                # TEST,STATUS,SECONDS; a piglit name can hold commas, which
                # piglit-runner reports as "-"
                parts = line.strip().rsplit(",", 2)
                if len(parts) == 3 and parts[0]:
                    status[parts[0].replace(",", "-")] = parts[1]
    wedges = []
    if a.wedges:
        with open(a.wedges) as f:
            wedges = sorted({t.strip() for t in f if t.strip() and not t.startswith("#")})
    reasons = []
    if a.reasons:
        with open(a.reasons) as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    rx, why = line.rstrip("\n").split("\t", 1)
                    reasons.append((re.compile(rx), why))

    counts = collections.Counter(s for t, s in status.items() if t not in wedges)
    head = (f"# {time.strftime('%Y-%m-%d')}: {len(status)} tests, "
            + ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
            + (f"\n# {a.note}" if a.note else "") + "\n")
    fails = sorted(t for t, s in status.items()
                   if s in ("Fail", "Crash", "Timeout", "Warn", "Missing") and t not in wedges)
    seen = set()
    if a.flakes:
        with open(a.flakes) as f:
            seen = {t.strip() for t in f if t.strip() and not t.startswith("#")}
    flakes = sorted(t for t, s in status.items() if s == "Flake" or t in seen)
    fails = [t for t in fails if t not in seen]

    groups = collections.OrderedDict((why, []) for _, why in reasons)
    groups["not looked at yet"] = []
    missing = "no test program: piglit's GLES, EGL and window-system tests aren't built (build-piglit.sh)"
    for t in fails:
        why = missing if status[t] == "Missing" else next((w for rx, w in reasons if rx.search(t)),
                                                           "not looked at yet")
        groups.setdefault(why, [])
        groups[why].append(t)
    body = ""
    for why, tests in groups.items():
        if tests:
            body += f"\n# {why}\n" + "".join(f"{t},{status[t]}\n" for t in tests)

    with open(a.prefix + "-fails.txt", "w") as f:
        f.write(head + body)
    with open(a.prefix + "-flakes.txt", "w") as f:
        f.write(head + "".join(f"{t}\n" for t in flakes))
    with open(a.prefix + "-skips.txt", "w") as f:
        f.write(head + "# tests that wedged the GPU or the box (drive-deqp.py)\n" + "".join(f"{t}\n" for t in wedges))
    print(head.strip())
    for why, tests in groups.items():
        if tests:
            print(f"{len(tests):5}  {why}")
    print(f"{len(fails)} fails, {len(flakes)} flakes, {len(wedges)} skips")


if __name__ == "__main__":
    main()
