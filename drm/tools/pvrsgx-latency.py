#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Per-stage latency of powervr_sgx jobs from a trace (drm/tools/pvrsgx-trace.sh).

    pvrsgx-latency.py TRACE

Stages, per job (drm_sched's events and the driver's own):
    queue -> run        the scheduler: dependencies, the entity's turn, its worker
    run -> kick         the driver: commands built, imports flushed, the CCB doorbell
    kick -> irq         the GPU and the microkernel, to the job's last interrupt
    irq -> done         the interrupt thread finding the job done (imports flushed again)
    done -> finished    the hardware fence's callback to the scheduler's finished fence
"""
import re
import statistics
import sys

LINE = re.compile(r"^\s*(?P<task>.+?)-(?P<pid>\d+)\s+\[\d+\]\s+\S+\s+(?P<ts>[\d.]+):\s+(?P<ev>\w+):\s*(?P<args>.*)$")


def parse(path):
    for line in open(path):
        m = LINE.match(line)
        if m:
            yield float(m["ts"]) * 1e6, m["ev"], m["args"], m["task"]


def stats(name, xs):
    if not xs:
        print(f"  {name:18s}  -")
        return
    xs = sorted(xs)
    p = lambda q: xs[min(len(xs) - 1, int(q * len(xs)))]
    print(f"  {name:18s}  n={len(xs):4d}  median {statistics.median(xs):8.1f} us  "
          f"p90 {p(0.9):8.1f}  min {xs[0]:8.1f}  max {xs[-1]:8.1f}")


def main():
    ev = list(parse(sys.argv[1]))
    queued, run_at, sched_of_run = {}, {}, []
    kicks = {}            # hw fence seqno -> (ts, type)
    done = {}             # hw fence seqno -> ts
    finished = {}         # sched fence -> ts
    irqs = []
    flush = {"before": [], "after": []}
    flush_bytes = []
    wakeups = 0
    last_run = None
    run_to_kick, sched_for_hw = [], {}
    for ts, name, args, task in ev:
        if name in ("drm_sched_job_queue", "drm_sched_job_run", "drm_sched_job_done"):
            m = re.search(r"fence=(\d+:\d+)", args)
            if not m:
                continue
            f = m.group(1)
            if name == "drm_sched_job_queue":
                queued[f] = ts
            elif name == "drm_sched_job_run":
                run_at[f] = ts
                last_run = f
            else:
                finished[f] = ts
        elif name == "pvrsgx_kick":
            t = int(re.search(r"type=(\d+)", args).group(1))
            s = int(re.search(r"fence=(\d+)", args).group(1))
            kicks[s] = (ts, t)
            if last_run is not None:
                sched_for_hw[s] = last_run
                last_run = None
        elif name == "pvrsgx_irq":
            irqs.append(ts)
        elif name == "pvrsgx_job_done":
            s = int(re.search(r"fence=(\d+)", args).group(1))
            done[s] = ts
        elif name == "pvrsgx_flush_imports":
            when = "after" if " after " in args else "before"
            flush[when].append(int(re.search(r"ns=(\d+)", args).group(1)) / 1000)
            flush_bytes.append(int(re.search(r"bytes=(\d+)", args).group(1)))
        elif name == "sched_wakeup" and "powervr" in args:
            wakeups += 1

    types = {0: "render", 1: "transfer", 2: "NULL"}
    by_type = {}
    for s, (kts, t) in sorted(kicks.items()):
        d = by_type.setdefault(t, {k: [] for k in ("q-run", "run-kick", "kick-irq", "irq-done",
                                                   "done-fin", "total")})
        sf = sched_for_hw.get(s)
        if sf in queued and sf in run_at:
            d["q-run"].append(run_at[sf] - queued[sf])
        if sf in run_at:
            d["run-kick"].append(kts - run_at[sf])
        if s in done:
            # the job's last interrupt: earlier ones (TA done, ...) don't complete it
            irq = max((i for i in irqs if kts <= i <= done[s]), default=None)
            n_irq = sum(1 for i in irqs if kts <= i <= done[s])
            d.setdefault("irqs", []).append(n_irq)
            if irq is not None:
                d["kick-irq"].append(irq - kts)
                d["irq-done"].append(done[s] - irq)
            if sf in finished:
                d["done-fin"].append(finished[sf] - done[s])
            if sf in queued and sf in finished:
                d["total"].append(finished[sf] - queued[sf])
    for t, d in sorted(by_type.items()):
        print(f"{types.get(t, t)} jobs:")
        for k, label in (("q-run", "queue -> run"), ("run-kick", "run -> kick"),
                         ("kick-irq", "kick -> irq"), ("irq-done", "irq -> done"),
                         ("done-fin", "done -> finished"), ("total", "queue -> finished")):
            stats(label, d[k])
        if d.get("irqs"):
            print(f"  interrupts per job  median {statistics.median(d['irqs'])}")
    if flush_bytes:
        print(f"import flushes: {len(flush_bytes)}, {statistics.median(flush_bytes) / 1024:.0f} KiB median")
        stats("flush before", flush["before"])
        stats("flush after", flush["after"])
    print(f"interrupt-thread wakeups: {wakeups} for {len(kicks)} jobs, {len(irqs)} interrupts")


if __name__ == "__main__":
    main()
