#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
"""Run dEQP-GLES2 on an EA from the host, across wedges and power cycles.

    drive-deqp.py sgx545|ddk [--box 10.0.0.113] [--chunk 200] [--filter REGEX] [--max-wedges N]
                  [--name NAME] [--caselist FILE]

--name keeps a run apart from the others (its results in run-NAME, its Mesa in
lib-NAME on the box), e.g. a rerun of one build's fails with the next build;
--caselist runs those tests instead of the mustpass list.

run-deqp.sh runs the whole caselist in one go on the box, which is fine once
nothing wedges. This is for the runs before that: the caselist goes to the box
a chunk at a time under deqp-runner, and every chunk's results.csv comes back
to this machine straight away, into $DEQP_WORK/run-<who>/results.csv (default
~/.cache/sgx545/deqp), so a restart carries on from the first test without a
result. A chunk that leaves the box or the GPU unresponsive is a wedge: the
RECOVER command power-cycles and boots the box again, the chunk is halved, and
a single test that still wedges goes into wedges.txt as a result of "Wedge" and
is not run again. Those are the lines for ci/<who>-skips.txt.

The box needs nothing but SSH: the payload ($DEQP_WORK/out, and our Mesa from
$MESA_DIR for sgx545) is copied to $BOX_DIR (default /root/deqp) when missing.

Environment:
  RECOVER     shell command that power-cycles the box and returns once it has
              booted and its GPU is up (default: mesa/tests/deqp/recover-ea3.sh)
  MESA_DIR    our x86_64 Mesa's lib directory on this machine
  DDK_ROOT    the DDK's i386 root on the box (default /opt/sgx-i386)
"""
import argparse
import os
import re
import shlex
import subprocess
import sys
import tarfile
import io
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get("DEQP_WORK", os.path.expanduser("~/.cache/sgx545/deqp"))
BOX_DIR = os.environ.get("BOX_DIR", "/root/deqp")
DDK_ROOT = os.environ.get("DDK_ROOT", "/opt/sgx-i386")
CONFIG = "rgba8888d24s8ms0"
TEST_TIMEOUT = 60  # deqp-runner's per-test limit, seconds
SSH_TIMEOUT = 25
RUN_DIR = None


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Box:
    def __init__(self, host):
        self.host = host

    def ssh(self, cmd, timeout=SSH_TIMEOUT, stdin=None):
        """(returncode, output); returncode None when the box didn't answer in time."""
        args = ["sshpass", "-p", "openhc", "ssh", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "root@" + self.host, cmd]
        try:
            r = subprocess.run(args, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None, b""
        return (None if r.returncode == 255 else r.returncode), r.stdout

    def alive(self, tries=3):
        for _ in range(tries):
            rc, out = self.ssh("echo up")
            if rc == 0 and b"up" in out:
                return True
            time.sleep(3)
        return False


def recoveries(box):
    rc, out = box.ssh("dmesg | grep -c 'Recovery triggered'")
    return int(out.strip() or 0) if rc in (0, 1) else None


def payload(box, who, name):
    """Copy what the run needs when it isn't there (a RAM root loses it at every boot)."""
    rc, _ = box.ssh(f"test -x {BOX_DIR}/deqp-runner && test -d {BOX_DIR}/gles2 && test -f {BOX_DIR}/.complete-{name}")
    if rc == 0:
        return
    log("copying the payload to the box")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as t:
        out = os.path.join(WORK, "out")
        t.add(os.path.join(out, "deqp-runner"), "deqp-runner")
        t.add(os.path.join(out, "gles2"), "gles2")
        if who == "sgx545":
            t.add(os.path.join(out, "deqp-gles2"), "deqp-gles2")
            t.add(os.environ["MESA_DIR"], "lib-" + name)
        else:
            t.add(os.path.join(out, "deqp-gles2-i386"), "deqp-gles2-i386")
    rc, o = box.ssh(f"mkdir -p {BOX_DIR} && cd {BOX_DIR} && tar xf - && touch .complete-{name}", timeout=900,
                    stdin=buf.getvalue())
    if rc != 0:
        sys.exit(f"payload copy failed: {o.decode(errors='replace')}")


def runner_script(who, name):
    if who == "sgx545":
        env = f"LD_LIBRARY_PATH={BOX_DIR}/lib-{name} EGL_PLATFORM=surfaceless SGX545_DEBUG=hangabort"
        deqp = f"{BOX_DIR}/deqp-gles2"
    else:
        deqp = f"{BOX_DIR}/deqp-gles2-ddk"
        env = ""
    wrap = ""
    if who == "ddk":
        wrap = (f"printf '#!/bin/sh\\ncd {BOX_DIR}\\nLD_PRELOAD={DDK_ROOT}/usr/lib/sgx-busidshim.so exec "
                f"{DDK_ROOT}/lib/ld-linux.so.2 --library-path {DDK_ROOT}/usr/lib:{DDK_ROOT}/lib "
                f"{BOX_DIR}/deqp-gles2-i386 \"$@\"\\n' > {deqp}; chmod +x {deqp}; ")
    return (f"{wrap}cd {BOX_DIR} && rm -rf out && {env} flock /tmp/gpu.lock ./deqp-runner run --deqp {deqp} "
            f"--caselist chunk.txt --output out --jobs 1 --timeout {TEST_TIMEOUT} -- "
            f"--deqp-surface-type=pbuffer --deqp-surface-width=256 --deqp-surface-height=256 "
            f"--deqp-gl-config-name={CONFIG} --deqp-visibility=hidden > runner.log 2>&1; echo rc=$?")


def canary(box, who, name):
    """Does a one-draw test still finish? A GPU that recovered does; a wedged one doesn't."""
    # (busybox has no timeout: a hung canary is the SSH call timing out)
    if who == "sgx545":
        pre = f"LD_LIBRARY_PATH={BOX_DIR}/lib-{name} EGL_PLATFORM=surfaceless"
        exe = f"{BOX_DIR}/deqp-gles2"
    else:
        pre = ""
        exe = f"{BOX_DIR}/deqp-gles2-ddk"
    rc, out = box.ssh(f"cd {BOX_DIR} && env {pre} {exe} "
                      f"--deqp-case=dEQP-GLES2.functional.color_clear.single_rgb --deqp-surface-type=pbuffer "
                      f"--deqp-surface-width=256 --deqp-surface-height=256 --deqp-gl-config-name={CONFIG} "
                      f"--deqp-visibility=hidden --deqp-log-filename=/dev/null 2>&1 | grep 'Passed:'", timeout=45)
    return rc == 0 and b"Passed:        1/1" in out


def recover(box):
    cmd = os.environ.get("RECOVER", shlex.quote(os.path.join(HERE, "recover-ea3.sh")))
    log("wedged: recovering the box:", cmd)
    subprocess.run(cmd, shell=True, check=False)
    deadline = time.time() + 600
    while time.time() < deadline:
        if box.alive(1):
            return True
        time.sleep(5)
    return False


def run_chunk(box, who, name, tests):
    """(results {test: (status, seconds)}, wedged)"""
    rc, _ = box.ssh(f"cat > {BOX_DIR}/chunk.txt", stdin=("\n".join(tests) + "\n").encode())
    if rc != 0:
        return {}, True
    limit = 120 + len(tests) * 4 + 3 * TEST_TIMEOUT
    limit = min(limit, 3600)
    r0 = recoveries(box)
    rc, out = box.ssh(runner_script(who, name), timeout=limit)
    res = {}
    wedged = rc is None
    if not wedged:
        rc2, csv = box.ssh(f"cat {BOX_DIR}/out/results.csv 2>/dev/null")
        if rc2 is None:
            wedged = True
        for line in csv.decode(errors="replace").splitlines():
            parts = line.split(",")
            if len(parts) >= 2 and parts[0].startswith("dEQP"):
                res[parts[0]] = (parts[1], parts[2] if len(parts) > 2 else "")
    if not wedged:
        # the failing tests' logs, for triage
        rc3, tar = box.ssh(f"cd {BOX_DIR}/out && tar cf - *.qpa 2>/dev/null", timeout=120)
        if rc3 is not None and tar:
            try:
                with tarfile.open(fileobj=io.BytesIO(tar)) as t:
                    t.extractall(os.path.join(RUN_DIR, "logs"), filter="data")
            except tarfile.TarError:
                pass
    r1 = recoveries(box) if not wedged else None
    if r0 is not None and r1 is not None and r1 != r0:
        log(f"chunk of {len(tests)}: {r1 - r0} SGX recoveries")
    if not wedged and not canary(box, who, name):
        log("the canary didn't pass after this chunk")
        wedged = True
    return res, wedged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("who", choices=["sgx545", "ddk"])
    ap.add_argument("--box", default=os.environ.get("EA3", "10.0.0.113"))
    ap.add_argument("--chunk", type=int, default=200)
    ap.add_argument("--filter", default=None)
    ap.add_argument("--max-wedges", type=int, default=40)
    ap.add_argument("--name", default=None)
    ap.add_argument("--caselist", default=None)
    a = ap.parse_args()
    box = Box(a.box)

    global RUN_DIR
    name = a.name or a.who
    run_dir = RUN_DIR = os.path.join(WORK, "run-" + name)
    os.makedirs(run_dir, exist_ok=True)
    res_path = os.path.join(run_dir, "results.csv")
    wedge_path = os.path.join(run_dir, "wedges.txt")
    done = set()
    if os.path.exists(res_path):
        with open(res_path) as f:
            done = {line.split(",")[0] for line in f if line.startswith("dEQP")}
    with open(a.caselist or os.path.join(WORK, "out/mustpass/gles2-main.txt")) as f:
        tests = [t.strip() for t in f if t.strip()]
    if a.filter:
        tests = [t for t in tests if re.search(a.filter, t)]
    todo = [t for t in tests if t not in done]
    log(f"{a.who}: {len(tests)} tests, {len(done)} have results, {len(todo)} to run")

    if not box.alive():
        sys.exit("the box doesn't answer")
    payload(box, a.who, name)
    if not canary(box, a.who, name):
        sys.exit("the canary doesn't pass on a fresh box: not starting")

    size = a.chunk
    wedges = 0
    res_file = open(res_path, "a")
    t0 = time.time()
    ran = 0
    while todo:
        chunk = todo[:size]
        res, wedged = run_chunk(box, a.who, name, chunk)
        for t, (status, secs) in res.items():
            res_file.write(f"{t},{status},{secs}\n")
        res_file.flush()
        finished = [t for t in chunk if t in res]
        if not wedged and not finished:
            # the runner failed outright; never spin on it
            rc, tail = box.ssh(f"tail -5 {BOX_DIR}/runner.log")
            sys.exit(f"no results from a chunk and no wedge:\n{tail.decode(errors='replace')}")
        ran += len(finished)
        todo = [t for t in todo if t not in res]
        if wedged:
            wedges += 1
            # everything the chunk finished before it wedged is kept; what's left is
            # retried smaller, down to the one test that does it
            rest = [t for t in chunk if t not in res]
            if len(rest) == 1 or size == 1:
                culprit = rest[0]
                log("WEDGE:", culprit)
                with open(wedge_path, "a") as f:
                    f.write(culprit + "\n")
                res_file.write(f"{culprit},Wedge,\n")
                res_file.flush()
                todo.remove(culprit)
                size = a.chunk
            else:
                size = max(1, len(rest) // 2)
            if wedges > a.max_wedges:
                sys.exit(f"{wedges} wedges: stopping; rerun to carry on")
            if not recover(box):
                sys.exit("the box didn't come back: stopping; rerun to carry on")
            payload(box, a.who, name)
        elif size < a.chunk:
            size = min(a.chunk, size * 2)
        rate = ran / max(time.time() - t0, 1)
        log(f"{len(todo)} left, {ran} run, {rate:.2f} tests/s, {wedges} wedges, chunk {size}")
    log("done")


if __name__ == "__main__":
    main()
