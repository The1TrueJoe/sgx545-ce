# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
"""Summarize lag-run.sh's output: per variant and workload, where the time went.

    python3 lag-analyze.py OUT

For each workload window (the LAG start/end markers):
  - GPU: each job's service time (from the later of its kick and the previous
    job's completion, to its completion: the GPU runs one job at a time), summed
    per process, as a share of the window; renders a second and their sizes;
  - CPU: each process's share of one core, and the whole machine's;
  - the driver's SGX_DEBUG=stats lines for X and xfwm4: frames a second, the
    draw and flush+wait milliseconds, the slot waits, CPU blits, render
    latency;
  - the workload's own metric (drag seconds, terminal lines a second, ...).
"""
import collections
import os
import re
import sys

TRACE = re.compile(r"^\s*(.+?)-(\d+)\s+\[\d+\].*?\s([\d.]+): (\w+): (.*)$")


def windows_from_trace(lines):
    out, open_ = [], {}
    for ln in lines:
        m = TRACE.match(ln)
        if not m or m.group(4) != "tracing_mark_write":
            continue
        f = m.group(5).split()
        if len(f) >= 4 and f[0] == "LAG":
            if f[1] == "start":
                open_[f[3]] = float(m.group(3))
            elif f[1] == "end" and f[3] in open_:
                out.append((f[3], open_.pop(f[3]), float(m.group(3))))
    return out


def gpu_jobs(lines):
    """(comm, kick time, done time) per job, matched in order by (type, fence)"""
    pending = collections.defaultdict(collections.deque)
    jobs, sizes = [], collections.defaultdict(collections.Counter)
    # the kick runs in drm_sched's worker: drm_sched_job (in the submitter)
    # and drm_run_job (in the worker, just before the kick) name the job
    submitter, running = {}, {}
    for ln in lines:
        m = TRACE.match(ln)
        if not m:
            continue
        comm, pid, t, ev, rest = (m.group(1).strip(), m.group(2), float(m.group(3)), m.group(4),
                                  m.group(5))
        if ev in ("drm_sched_job", "drm_run_job"):
            k = re.search(r"\bid=(\d+)", rest) or re.search(r"fence=(\d+:\d+)", rest)
            if k:
                if ev == "drm_sched_job":
                    submitter[k.group(1)] = comm
                else:
                    running[pid] = submitter.get(k.group(1))
            continue
        kv = dict(x.rstrip(",").split("=", 1) for x in rest.split() if "=" in x)
        if ev == "pvrsgx_kick":
            owner = running.get(pid) or comm
            pending[(kv.get("type"), kv.get("fence"))].append((owner, t))
        elif ev == "pvrsgx_job_done":
            q = pending.get((kv.get("type"), kv.get("fence")))
            if q:
                c, t0 = q.popleft()
                jobs.append((c, t0, t))
        elif ev == "pvrsgx_render_kick":
            sz = re.search(r"\s(\d+x\d+)\s", rest)
            if sz:
                sizes[running.get(pid) or comm][sz.group(1)] += 1
    jobs.sort(key=lambda j: j[2])
    served, prev = [], 0.0
    for c, t0, t1 in jobs:
        start = max(t0, prev)
        served.append((c, start, t1, t1 - t0))
        prev = t1
    return served, sizes


def overlap(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def cpu_shares(path, marks, clk):
    """per workload: {comm: share of one core}, and the machine's busy share"""
    samples = []
    for ln in open(path):
        f = ln.split()
        if f[0] == "T":
            samples.append(("T", float(f[1]), [int(x) for x in f[3:]]))
        elif f[0] == "P" and len(f) >= 6:
            samples.append(("P", float(f[1]), (int(f[2]), int(f[3]) + int(f[4]), f[5])))
    res = {}
    for w, t0, t1 in marks:
        first, last = {}, {}
        tot = []
        for kind, t, v in samples:
            if not t0 <= t <= t1:
                continue
            if kind == "T":
                tot.append(v)
            else:
                pid, ticks, comm = v
                first.setdefault(pid, (t, ticks, comm))
                last[pid] = (t, ticks, comm)
        per = collections.Counter()
        for pid, (ta, ka, comm) in first.items():
            tb, kb, _ = last[pid]
            if tb > ta:
                per[comm] += (kb - ka) / clk / (tb - ta)
        busy = None
        if len(tot) >= 2:
            a, b = tot[0], tot[-1]
            idle = (b[3] + b[4]) - (a[3] + a[4])
            allt = sum(b) - sum(a)
            busy = 1 - idle / allt if allt else None
        res[w] = (per, busy)
    return res


NUM = r"([\d.]+)"


def stats(path):
    """the averages of a stats file's lines"""
    if not os.path.exists(path):
        return {}
    acc = collections.defaultdict(list)
    for ln in open(path):
        m = re.match(r"sgx stats " + NUM + r" s: " + NUM + r" FPS, (\d+) flushes.*?(\d+) draws.*draw " + NUM +
                     r", flush\+wait " + NUM, ln)
        if m:
            acc["fps"].append(float(m.group(2)))
            acc["draw_ms"].append(float(m.group(5)) / float(m.group(1)))
            acc["flushwait_ms"].append(float(m.group(6)) / float(m.group(1)))
            continue
        m = re.search(r"thread CPU " + NUM + r" ms of " + NUM + r".*slot wait " + NUM + r", sync wait " + NUM, ln)
        if m:
            acc["cpu_ms_s"].append(float(m.group(1)) / (float(m.group(2)) / 1000))
            acc["slot_wait_ms_s"].append(float(m.group(3)) / (float(m.group(2)) / 1000))
            continue
        m = re.search(r"blits: GPU (\d+), copy (\d+), CPU (\d+) \((\d+) px, " + NUM + r" ms\)", ln)
        if m:
            acc["cpu_blits"].append(int(m.group(3)))
            acc["cpu_blit_ms"].append(float(m.group(5)))
            continue
        m = re.search(r"render latency \(kick to seen done\) " + NUM + r" ms over (\d+)", ln)
        if m and int(m.group(2)):
            acc["latency_ms"].append(float(m.group(1)))
    return {k: sum(v) / len(v) for k, v in acc.items() if v}


def metric(path):
    out = {}
    if os.path.exists(path):
        for ln in open(path):
            f = ln.split()
            if len(f) == 3 and f[0] == "metric":
                out[f[1]] = f[2]
    return out


def fmt(x, p=0):
    return "-" if x is None else f"{x:.{p}f}"


def variant(d, name):
    tpath = os.path.join(d, "trace.txt")
    if not os.path.exists(tpath):
        print(f"## {name}: no trace\n")
        return
    lines = open(tpath, errors="replace").read().splitlines()
    marks = windows_from_trace(lines)
    jobs, sizes = gpu_jobs(lines)
    clk = int(open(os.path.join(d, "clk_tck.txt")).read()) if os.path.exists(os.path.join(d, "clk_tck.txt")) else 100
    # cpu.txt and marks.txt share /proc/uptime's clock
    um = []
    if os.path.exists(os.path.join(d, "marks.txt")):
        o = {}
        for ln in open(os.path.join(d, "marks.txt")):
            f = ln.split()
            if f[1] == "start":
                o[f[3]] = float(f[0])
            elif f[1] == "end" and f[3] in o:
                um.append((f[3], o.pop(f[3]), float(f[0])))
    cpu = cpu_shares(os.path.join(d, "cpu.txt"), um, clk) if um else {}
    res = open(os.path.join(d, "xrandr.txt")).read() if os.path.exists(os.path.join(d, "xrandr.txt")) else ""
    cur = re.search(r"current (\d+ x \d+)", res)
    print(f"## {name} ({cur.group(1) if cur else '?'})\n")
    print("| workload | s | GPU busy % (by process) | renders/s | CPU % Xorg / xfwm4 / other / machine | "
          "X FPS, draw+flush ms/s, slot wait ms/s, CPU blits | xfwm4 FPS, draw+flush ms/s, latency ms | metric |")
    print("|---|---|---|---|---|---|---|---|")
    for w, t0, t1 in marks:
        dur = t1 - t0
        busy = collections.Counter()
        n = collections.Counter()
        for c, s, e, _ in jobs:
            ov = overlap(s, e, t0, t1)
            if ov:
                busy[c] += ov
            if t0 <= e <= t1:
                n[c] += 1
        gb = ", ".join(f"{c} {100 * b / dur:.0f}" for c, b in busy.most_common(3)) or "0"
        rn = ", ".join(f"{c} {k / dur:.0f}" for c, k in n.most_common(3)) or "0"
        per, mach = cpu.get(w, ({}, None))
        x = per.get("Xorg")
        f = per.get("xfwm4")
        other = sum(v for k, v in per.items() if k not in ("Xorg", "xfwm4"))
        cp = f"{fmt(x and 100 * x)} / {fmt(f and 100 * f)} / {fmt(100 * other)} / {fmt(mach and 100 * mach)}"
        sx = stats(os.path.join(d, f"stats-x-{w}.txt"))
        sf = stats(os.path.join(d, f"stats-xfwm4-{w}.txt"))
        xs = (f"{fmt(sx.get('fps'), 1)}, {fmt((sx.get('draw_ms') or 0) + (sx.get('flushwait_ms') or 0))}, "
              f"{fmt(sx.get('slot_wait_ms_s'))}, {fmt(sx.get('cpu_blits'))}") if sx else "-"
        fs = (f"{fmt(sf.get('fps'), 1)}, {fmt((sf.get('draw_ms') or 0) + (sf.get('flushwait_ms') or 0))}, "
              f"{fmt(sf.get('latency_ms'), 1)}") if sf else "-"
        mt = ", ".join(f"{k} {v}" for k, v in metric(os.path.join(d, f"wl-{w}.txt")).items()) or ""
        print(f"| {w} | {dur:.0f} | {gb} | {rn} | {cp} | {xs} | {fs} | {mt} |")
    if sizes:
        print("\nrender sizes: " + "; ".join(f"{c}: " + ", ".join(f"{s} x{k}" for s, k in v.most_common(3))
                                       for c, v in sizes.items()))
    for note in ("notes.txt", "libs.txt"):
        p = os.path.join(d, note)
        if os.path.exists(p):
            print(f"\n{note}:\n```\n{open(p).read().strip()}\n```")
    print()


def main():
    out = sys.argv[1]
    for name in sorted(os.listdir(out)):
        d = os.path.join(out, name)
        if os.path.isdir(d):
            variant(d, name)


if __name__ == "__main__":
    main()
