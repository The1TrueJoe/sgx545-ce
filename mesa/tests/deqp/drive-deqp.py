#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
"""Run dEQP-GLES2 (or piglit) on an EA from the host, across wedges and power cycles.

    drive-deqp.py sgx545|ddk|piglit [--box 10.0.0.113] [--chunk 200] [--filter REGEX] [--max-wedges N]
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

piglit runs piglit's gpu profile (quick_gl and quick_shader; mesa/tests/piglit/
build-piglit.sh) on our desktop-GL Mesa ($MESA_DIR: the x86_64-gl build's lib)
through piglit-runner, which takes no caselist: each chunk is passed as anchored
--include-tests regexes. Its payload is $PIGLIT_WORK/out (default
~/.cache/sgx545/piglit), in $BOX_DIR (default /root/piglit).

Environment:
  RECOVER     shell command that power-cycles the box and returns once it has
              booted and its GPU is up (default: mesa/tests/deqp/recover-ea3.sh);
              "none" stops the run at the first wedge instead
  MESA_DIR    our x86_64 Mesa's lib directory on this machine
  DDK_ROOT    the DDK's i386 root on the box (default /opt/sgx-i386)
  BOX_ENV     more variables for our driver on the box, e.g. "SGX545_F16=0"
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
PIGLIT_WORK = os.environ.get("PIGLIT_WORK", os.path.expanduser("~/.cache/sgx545/piglit"))
BOX_DIR = os.environ.get("BOX_DIR", "/root/deqp")    # /root/piglit for piglit (main)
DDK_ROOT = os.environ.get("DDK_ROOT", "/opt/sgx-i386")
BOX_ENV = os.environ.get("BOX_ENV", "")
CONFIG = "rgba8888d24s8ms0"
TEST_TIMEOUT = 60  # deqp-runner's per-test limit, seconds
SSH_TIMEOUT = 25
RUN_DIR = None
LOCK = "flock /tmp/gpu.lock"  # per chunk; empty with --hold-lock


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Box:
    def __init__(self, host):
        self.host = host

    def ssh(self, cmd, timeout=SSH_TIMEOUT, stdin=None):
        """(returncode, output); returncode None when the box didn't answer in time."""
        args = ["sshpass", "-p", "openhc", "ssh", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "root@" + self.host, cmd]
        for attempt in range(4):
            t0 = time.time()
            try:
                r = subprocess.run(args, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   timeout=timeout)
            except subprocess.TimeoutExpired:
                return None, b""
            # a login refused at once (the EA1 does that now and then) is not a wedge
            if r.returncode != 255 or time.time() - t0 > 15:
                break
            time.sleep(3)
        return (None if r.returncode == 255 else r.returncode), r.stdout

    def alive(self, tries=3):
        for _ in range(tries):
            rc, out = self.ssh("echo up")
            if rc == 0 and b"up" in out:
                return True
            time.sleep(3)
        return False


STATUSES = {"Pass", "Fail", "Skip", "Crash", "Timeout", "Flake", "Warn", "Missing", "Wedge",
            "ExpectedFail", "UnexpectedPass", "KnownFlake"}


def result_line(line):
    """(test, status, seconds) from a results.csv line; piglit's names may hold commas"""
    parts = line.strip().rsplit(",", 2)
    if len(parts) == 3 and parts[1] in STATUSES:
        return parts[0], parts[1], parts[2]
    if len(parts) >= 2 and parts[-1] in STATUSES:     # "test,Wedge,"-less forms
        return ",".join(parts[:-1]), parts[-1], ""
    return None


def recoveries(box):
    rc, out = box.ssh("dmesg | grep -c 'Recovery triggered'")
    return int(out.strip() or 0) if rc in (0, 1) else None


def piglit_pieces(out):
    """the payload as resumable pieces of a few MB: (marker name, paths relative to out)"""
    pieces = [("top", [e for e in sorted(os.listdir(out)) if e != "piglit"])]

    def split(rel, batch):
        """a directory's subdirectories one piece each, its files in batches"""
        d = os.path.join(out, rel)
        entries = sorted(os.listdir(d))
        files = [f"{rel}/{e}" for e in entries if not os.path.isdir(os.path.join(d, e))]
        dirs = [e for e in entries if os.path.isdir(os.path.join(d, e))]
        tag = rel.replace("/", "-")
        for i in range(0, len(files), batch):
            pieces.append((f"{tag}-files{i // batch}", files[i:i + batch]))
        return dirs

    for e in split("piglit", 1000):
        if e in ("tests", "generated_tests"):
            for sub in split(f"piglit/{e}", 1000):
                if sub == "spec":    # most of the payload: one piece per extension or version
                    for t in split(f"piglit/{e}/spec", 1000):
                        pieces.append((f"{e}-spec-{t}", [f"piglit/{e}/spec/{t}"]))
                else:
                    pieces.append((f"{e}-{sub}", [f"piglit/{e}/{sub}"]))
        elif e == "bin":
            split("piglit/bin", 200)
        else:
            pieces.append((f"piglit-{e}", [f"piglit/{e}"]))
    return pieces


def piglit_payload(box, name):
    """piglit's tree, glvnd and waffle once, in pieces a reboot can't spoil (each one
    marked done on the box once unpacked: a copy cut off resumes); our Mesa
    (libEGL_mesa, libgallium) per run name"""
    out = os.path.join(PIGLIT_WORK, "out")
    rc, done = box.ssh(f"mkdir -p {BOX_DIR}/.pieces && ls {BOX_DIR}/.pieces")
    if rc != 0:
        log("box busy: it doesn't answer")
        sys.exit(4)
    done = set(done.decode().split())
    todo = [(m, paths) for m, paths in piglit_pieces(out) if m not in done]
    if todo:
        log(f"copying piglit to the box: {len(todo)} pieces")
    for i, (marker, paths) in enumerate(todo):
        tar = subprocess.run(["tar", "czf", "-", "-C", out] + paths, stdout=subprocess.PIPE, check=True,
                             env=dict(os.environ, COPYFILE_DISABLE="1"))
        rc, o = box.ssh(f"cd {BOX_DIR} && gzip -dc | tar xf - && touch .pieces/{shlex.quote(marker)}",
                        timeout=900, stdin=tar.stdout)
        if rc != 0:
            # (a reboot of someone else's, usually: the next try carries on)
            log(f"box busy: piglit copy stopped at {marker} ({i} of {len(todo)} done); "
                f"{o.decode(errors='replace').strip()}")
            sys.exit(4)
    rc, _ = box.ssh(f"test -f {BOX_DIR}/lib-{name}/50_mesa.json")
    if rc == 0:
        return
    log("copying our Mesa to the box")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as t:
        t.add(os.environ["MESA_DIR"], "lib-" + name)
        icd = (f'{{"file_format_version":"1.0.0","ICD":{{"library_path":"{BOX_DIR}/lib-{name}/libEGL_mesa.so.0"}}}}\n'
               .encode())
        ti = tarfile.TarInfo(f"lib-{name}/50_mesa.json")
        ti.size = len(icd)
        t.addfile(ti, io.BytesIO(icd))
    rc, o = box.ssh(f"cd {BOX_DIR} && tar xf -", timeout=900, stdin=buf.getvalue())
    if rc != 0:
        sys.exit(f"payload copy failed: {o.decode(errors='replace')}")


def piglit_env(name):
    # glvnd dispatches to this run's Mesa; it stays loaded, since unloading it at
    # waffle's teardown crashes the process at exit (docs/PIGLIT.md)
    lib = f"{BOX_DIR}/lib-{name}"
    return (f"LD_LIBRARY_PATH={lib}:{BOX_DIR}/lib:{BOX_DIR}/piglit/lib __EGL_VENDOR_LIBRARY_FILENAMES={lib}/50_mesa.json "
            f"LD_PRELOAD={lib}/libEGL_mesa.so.0 PIGLIT_PLATFORM=surfaceless_egl PIGLIT_NO_WINDOW=1 {BOX_ENV}")


def piglit_regex(test):
    """a test name as an anchored regex; only regex syntax is escaped (Rust's regex rejects \\@)"""
    return "^" + re.sub(r"([\\.+*?()|\[\]{}^$#&~-])", r"\\\1", test) + "$"


def piglit_gpu_tests():
    """the gpu profile's tests (tests/gpu.meta.xml: quick_gl and quick_shader), in profile order"""
    import gzip
    import xml.etree.ElementTree as ET

    tests = []
    for prof in ("quick_gl", "quick_shader"):
        with gzip.open(os.path.join(PIGLIT_WORK, "out/piglit/tests", prof + ".xml.gz")) as f:
            tests += [t.get("name") for t in ET.parse(f).getroot().iter("Test")]
    # GLX needs X: on the surfaceless platform its tests don't exist (not built)
    return [t for t in tests if not t.startswith("glx@")]


def payload(box, who, name):
    """Copy what the run needs when it isn't there (a RAM root loses it at every boot)."""
    if who == "piglit":
        return piglit_payload(box, name)
    rc, _ = box.ssh(f"test -x {BOX_DIR}/deqp-runner && test -d {BOX_DIR}/gles2 && test -f {BOX_DIR}/.complete-{name}")
    if rc == 0:
        return
    # dEQP itself is there from an earlier run: only a new Mesa to send
    exe = "deqp-gles2" if who == "sgx545" else "deqp-gles2-i386"
    rc, _ = box.ssh(f"test -x {BOX_DIR}/deqp-runner && test -d {BOX_DIR}/gles2 && test -x {BOX_DIR}/{exe}")
    whole = rc != 0
    log("copying the payload to the box" if whole else "copying our Mesa to the box")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as t:
        out = os.path.join(WORK, "out")
        if whole:
            t.add(os.path.join(out, "deqp-runner"), "deqp-runner")
            t.add(os.path.join(out, "gles2"), "gles2")
            t.add(os.path.join(out, exe), exe)
        if who == "sgx545":
            t.add(os.environ["MESA_DIR"], "lib-" + name)
    rc, o = box.ssh(f"mkdir -p {BOX_DIR} && cd {BOX_DIR} && tar xf - && touch .complete-{name}", timeout=900,
                    stdin=buf.getvalue())
    if rc != 0:
        sys.exit(f"payload copy failed: {o.decode(errors='replace')}")


def runner_script(who, name):
    if who == "piglit":
        return (f"cd {BOX_DIR} && rm -rf out && set -- && while IFS= read -r t; do set -- \"$@\" "
                f"--include-tests \"$t\"; done < chunk.txt && env {piglit_env(name)} SGX545_DEBUG=hangabort "
                f"{LOCK} ./piglit-runner run --piglit-folder {BOX_DIR}/piglit --profile gpu --process-isolation "
                f"--output out --jobs 1 --timeout {TEST_TIMEOUT} \"$@\" > runner.log 2>&1; echo rc=$?")
    if who == "sgx545":
        env = f"LD_LIBRARY_PATH={BOX_DIR}/lib-{name} EGL_PLATFORM=surfaceless SGX545_DEBUG=hangabort {BOX_ENV}"
        deqp = f"{BOX_DIR}/deqp-gles2"
    else:
        deqp = f"{BOX_DIR}/deqp-gles2-ddk"
        env = ""
    wrap = ""
    if who == "ddk":
        wrap = (f"printf '#!/bin/sh\\ncd {BOX_DIR}\\nLD_PRELOAD={DDK_ROOT}/usr/lib/sgx-busidshim.so exec "
                f"{DDK_ROOT}/lib/ld-linux.so.2 --library-path {DDK_ROOT}/usr/lib:{DDK_ROOT}/lib "
                f"{BOX_DIR}/deqp-gles2-i386 \"$@\"\\n' > {deqp}; chmod +x {deqp}; ")
    return (f"{wrap}cd {BOX_DIR} && rm -rf out && {env} {LOCK} ./deqp-runner run --deqp {deqp} "
            f"--caselist chunk.txt --output out --jobs 1 --timeout {TEST_TIMEOUT} -- "
            f"--deqp-surface-type=pbuffer --deqp-surface-width=256 --deqp-surface-height=256 "
            f"--deqp-gl-config-name={CONFIG} --deqp-visibility=hidden > runner.log 2>&1; echo rc=$?")


def canary(box, who, name):
    """Does a one-draw test still finish? A GPU that recovered does; a wedged one doesn't."""
    # (busybox has no timeout: a hung canary is the SSH call timing out)
    if who == "piglit":
        rc, out = box.ssh(f"cd {BOX_DIR}/piglit && env {piglit_env(name)} {LOCK} ./bin/shader_runner "
                          f"tests/spec/glsl-1.10/execution/glsl-fs-add-masked.shader_test -auto -fbo 2>&1",
                          timeout=45)
        return rc == 0 and b'"result": "pass"' in out
    if who == "sgx545":
        pre = f"LD_LIBRARY_PATH={BOX_DIR}/lib-{name} EGL_PLATFORM=surfaceless {BOX_ENV}"
        exe = f"{BOX_DIR}/deqp-gles2"
    else:
        pre = ""
        exe = f"{BOX_DIR}/deqp-gles2-ddk"
    rc, out = box.ssh(f"cd {BOX_DIR} && env {pre} {LOCK} {exe} "
                      f"--deqp-case=dEQP-GLES2.functional.color_clear.single_rgb --deqp-surface-type=pbuffer "
                      f"--deqp-surface-width=256 --deqp-surface-height=256 --deqp-gl-config-name={CONFIG} "
                      f"--deqp-visibility=hidden --deqp-log-filename=/dev/null 2>&1 | grep 'Passed:'", timeout=45)
    return rc == 0 and b"Passed:        1/1" in out


def recover(box):
    cmd = os.environ.get("RECOVER", shlex.quote(os.path.join(HERE, "recover-ea3.sh")))
    if cmd == "none":
        # a box without a smart plug: someone has to power-cycle it
        log("wedged, and RECOVER=none: stopping; power-cycle the box and rerun to carry on")
        return False
    log("wedged: recovering the box:", cmd)
    subprocess.run(cmd, shell=True, check=False)
    deadline = time.time() + 600
    while time.time() < deadline:
        if box.alive(1):
            return True
        time.sleep(5)
    return False


def run_chunk(box, who, name, tests):
    """(results {test: (status, seconds)}, wedged, SGX recoveries during it)"""
    lines = [piglit_regex(t) for t in tests] if who == "piglit" else tests
    rc, _ = box.ssh(f"cat > {BOX_DIR}/chunk.txt", stdin=("\n".join(lines) + "\n").encode())
    if rc != 0:
        return {}, True, 0
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
            r = result_line(line)
            if r:
                res[r[0]] = (r[1], r[2])
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
    return res, wedged, (r1 - r0 if r0 is not None and r1 is not None else 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("who", choices=["sgx545", "ddk", "piglit"])
    ap.add_argument("--box", default=os.environ.get("EA3", "10.0.0.113"))
    ap.add_argument("--chunk", type=int, default=200)
    ap.add_argument("--filter", default=None)
    ap.add_argument("--max-wedges", type=int, default=40)
    ap.add_argument("--name", default=None)
    ap.add_argument("--caselist", default=None)
    ap.add_argument("--max-recoveries", type=int, default=None,
                    help="stop after a chunk with more SGX recoveries than this (a box without a plug)")
    ap.add_argument("--slice-minutes", type=float, default=None,
                    help="stop (exit 3) after the chunk that passes this many minutes, to share the box")
    ap.add_argument("--copy-only", action="store_true", help="copy the payload and stop (no GPU work)")
    ap.add_argument("--hold-lock", action="store_true",
                    help="hold /tmp/gpu.lock on the box for the whole run, so nothing interleaves between chunks")
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
            done = {r[0] for r in map(result_line, f) if r}
    if a.who == "piglit":
        global BOX_DIR
        BOX_DIR = os.environ.get("BOX_DIR", "/root/piglit")
    if a.caselist:
        with open(a.caselist) as f:
            tests = [t.strip() for t in f if t.strip()]
    elif a.who == "piglit":
        tests = piglit_gpu_tests()
    else:
        with open(os.path.join(WORK, "out/mustpass/gles2-main.txt")) as f:
            tests = [t.strip() for t in f if t.strip()]
    if a.filter:
        tests = [t for t in tests if re.search(a.filter, t)]
    todo = [t for t in tests if t not in done]
    log(f"{a.who}: {len(tests)} tests, {len(done)} have results, {len(todo)} to run")

    if not box.alive():
        log("box busy: it doesn't answer (rebooting?)")
        sys.exit(4)
    payload(box, a.who, name)     # no GPU work: before the lock
    if a.copy_only:
        log("payload on the box")
        return
    if a.slice_minutes:
        busy = box_busy(box, run_dir)
        if busy:
            log("box busy:", busy)
            sys.exit(4)
    holder = None
    if a.hold_lock:
        # one SSH session keeps the lock while the marker exists; the chunks run without flock
        global LOCK
        LOCK = ""
        box.ssh("touch /tmp/deqp-job")
        holder = subprocess.Popen(["sshpass", "-p", "openhc", "ssh", "-o", "StrictHostKeyChecking=no",
                                   "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "root@" + a.box,
                                   "flock -n /tmp/gpu.lock sh -c 'echo locked; while [ -e /tmp/deqp-job ]; do sleep 2; done' "
                                   "|| echo busy"],
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if b"locked" not in holder.stdout.readline():
            box.ssh("rm -f /tmp/deqp-job")
            log("box busy: someone holds /tmp/gpu.lock")
            sys.exit(4)
        log("holding /tmp/gpu.lock for the run")
    try:
        run(a, box, name, todo, res_path, wedge_path)
    finally:
        if holder:
            box.ssh("rm -f /tmp/deqp-job")
            holder.wait(timeout=30)


def box_state(box):
    """(normal boot, boot id, SGX recoveries since boot), or None if the box doesn't answer"""
    rc, out = box.ssh("cat /proc/cmdline; cat /proc/sys/kernel/random/boot_id; "
                      "dmesg | grep -c 'Recovery triggered'")
    lines = out.decode(errors="replace").split("\n")
    if rc not in (0, 1) or len(lines) < 3:
        return None
    cmdline = lines[0]
    normal = "root=/dev/mmcblk0p1" in cmdline and "ohc.ramboot" not in cmdline
    return normal, lines[1].strip(), int(lines[2].strip() or 0)


def box_busy(box, run_dir):
    """Why the box is someone else's right now, or None: a slice starts only on the box's
    normal boot, with no SGX recoveries since it booted beyond what the last slice left"""
    st = box_state(box)
    if st is None:
        return "doesn't answer"
    normal, boot, rec = st
    if not normal:
        return "not on its normal boot (a test kernel or RAM image)"
    try:
        with open(os.path.join(run_dir, "box-state")) as f:
            last_boot, last_rec = f.read().split()
    except (OSError, ValueError):
        last_boot, last_rec = None, 0
    if rec and not (boot == last_boot and rec == int(last_rec)):
        return f"{rec} SGX recoveries since boot that aren't ours"
    return None


def save_box_state(box, run_dir):
    st = box_state(box)
    if st:
        with open(os.path.join(run_dir, "box-state"), "w") as f:
            f.write(f"{st[1]} {st[2]}\n")


def run(a, box, name, todo, res_path, wedge_path):
    if not canary(box, a.who, name):
        sys.exit("the canary doesn't pass on a fresh box: not starting")

    size = a.chunk
    wedges = 0
    res_file = open(res_path, "a")
    t0 = time.time()
    ran = 0
    while todo:
        if a.slice_minutes and time.time() - t0 > a.slice_minutes * 60:
            save_box_state(box, RUN_DIR)
            log(f"slice over: {len(todo)} left; rerun to carry on")
            sys.exit(3)
        chunk = todo[:size]
        res, wedged, nrec = run_chunk(box, a.who, name, chunk)
        for t, (status, secs) in res.items():
            res_file.write(f"{t},{status},{secs}\n")
        res_file.flush()
        if a.max_recoveries is not None and nrec > a.max_recoveries:
            sys.exit(f"{nrec} SGX recoveries in a chunk of {len(chunk)} ({chunk[0]} ..): stopping")
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
    save_box_state(box, RUN_DIR)
    log("done")


if __name__ == "__main__":
    main()
