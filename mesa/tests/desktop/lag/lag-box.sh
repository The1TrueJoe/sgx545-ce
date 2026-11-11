#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On the box, as root: one desktop variant through the lag workloads
# (lag-wl.sh), with what each costs recorded.
#
#   lag-box.sh ROOT RIG OUT VARIANT WORKLOAD...
#
# ROOT is the desktop root (desktop-up.sh), RIG a directory with desktop-up.sh,
# xorg.conf and lag-wl.sh, and ROOT/opt/sgx the Mesa under test. Variants:
#   default   Xorg with glamor, xfwm4 compositing through GLX (Xfce's default)
#   slots3    the same with SGX_SLOTS=3 for X and the clients
#   gpuimport the same with SGX_DEBUG=gpuimport: scanout buffers the GPU's,
#             imported by the display device (its damage copy reads them)
#   flatten   the same with the compiler's uniform-condition ifs flattened
#             into selects (SGX_COMPILER_DEBUG=flattenuniform): the A/B of
#             branching on them (glamor's repeat modes)
#   nolevers  the same without lazy clears and depth discards
#             (SGX_DEBUG=nolazyclear,nozsdiscard): their A/B
#   nopdsmerge  the same with SGX_DEBUG=nopdsmerge (the PDS-merge lever off)
#   nocomp    xfwm4 without its compositor
#   xpresent  xfwm4 synced to vblank through Present, not GLX
#   vbloff    xfwm4 not synced to vblank
#   xsw       LIBGL_ALWAYS_SOFTWARE for X only: glamor ignores it (still the
#             GPU, through GBM); kept for the record, not a control
#   dumbsched the same with Xorg -dumbSched (no scheduler timer signal)
#   serial    the same with SGX_DEBUG=serial: each render waited for, no
#             overlap (whether a render still in flight takes part)
#   clientsw  X with glamor on the GPU, the clients (glmark2, xfwm4) on the
#             root's own Mesa (llvmpipe): whether GPU clients take part
#   software  no GPU: Xorg without glamor, clients on the root's own Mesa
#             (llvmpipe), the baseline at the same resolution
#   glamordebug  the default with GLAMOR_DEBUG=1 (counts glamor's CPU
#             fallbacks; slower, so not for timing)
#   shaderdump  the default with MESA_SHADER_DUMP_PATH: X's, xfwm4's and the
#             clients' programs as .shader_test files in OUT/shaders (not for
#             timing)
# SCANOUT=address (and SCANOUT_BYTES) dumps the display's framebuffer after
# each workload with /root/m3/memdump.
# Recorded in OUT: the kernel's job trace (kick and completion per job, by
# process), CPU time per process every 0.5 s, the driver's SGX_DEBUG=stats
# lines of X and xfwm4, the workloads' own metrics, perf profiles when perf
# is there, and the X log.
set -u
R=$1 RIG=$2 OUT=$3 V=$4
shift 4
mkdir -p "$OUT"
T=/sys/kernel/tracing
[ -e $T/trace ] || T=/sys/kernel/debug/tracing

mesa="LD_LIBRARY_PATH=/opt/sgx/lib GBM_BACKENDS_PATH=/opt/sgx/lib/gbm \
__EGL_VENDOR_LIBRARY_DIRS=/opt/sgx/share/glvnd/egl_vendor.d __GLX_VENDOR_LIBRARY_NAME=mesa \
LIBGL_DRIVERS_PATH=/opt/sgx/lib/dri"
cenv="$mesa SGX_DEBUG=stats"
xenv="$mesa SGX_DEBUG=stats"
xfwm= xargs=
case $V in
default) ;;
slots3) cenv="$cenv SGX_SLOTS=3" xenv="$xenv SGX_SLOTS=3" ;;
gpuimport)   # scanout buffers the GPU's, imported by the display (simpledrm)
	cenv="$cenv,gpuimport" xenv="$xenv,gpuimport" ;;
flatten)   # the compiler's uniform ifs flattened into selects again
	cenv="$cenv SGX_COMPILER_DEBUG=flattenuniform"
	xenv="$xenv SGX_COMPILER_DEBUG=flattenuniform" ;;
nolevers)   # the default without lazy clears and depth discards (PERF.md levers 2, 3)
	cenv="$mesa SGX_DEBUG=stats,nolazyclear,nozsdiscard"
	xenv="$mesa SGX_DEBUG=stats,nolazyclear,nozsdiscard" ;;
nopdsmerge)   # samples merged into their own set's iteration only (the PDS-merge lever off)
	cenv="$cenv,nopdsmerge" xenv="$xenv,nopdsmerge" ;;
nocomp) xfwm=--compositor=off ;;
xpresent) xfwm=--vblank=xpresent ;;
vbloff) xfwm=--vblank=off ;;
software) cenv="LIBGL_ALWAYS_SOFTWARE=1" xenv= ;;
clientsw) cenv="LIBGL_ALWAYS_SOFTWARE=1" ;;   # X on the GPU, the clients on llvmpipe
xsw) xenv="$xenv LIBGL_ALWAYS_SOFTWARE=1" ;;   # glamor on llvmpipe: X off the GPU, the clients on it
dumbsched) xargs=-dumbSched ;;   # Xorg without the smart scheduler's timer signal
serial)   # every render waited for after its submit: nothing in flight when a buffer goes
	cenv="$cenv,serial" xenv="$xenv,serial" ;;
glamordebug) xenv="$xenv GLAMOR_DEBUG=1" ;;
shaderdump)   # every linked GL program as a .shader_test, for the compiler
	mkdir -p /tmp/lag-shaders; chmod 1777 /tmp/lag-shaders
	xenv="$xenv MESA_SHADER_DUMP_PATH=/tmp/lag-shaders"
	cenv="$cenv MESA_SHADER_DUMP_PATH=/tmp/lag-shaders" ;;
*) echo "lag-box.sh: no variant $V" >&2; exit 2 ;;
esac

as_desk() {   # a client in the session, as desk, with this variant's Mesa
	chroot "$R" /usr/bin/env -i HOME=/home/desk USER=desk DISPLAY=:0 PATH=/usr/local/bin:/usr/bin:/bin \
		XDG_RUNTIME_DIR=/tmp/desk-run $cenv /usr/sbin/runuser -u desk -- "$@"
}
mark() {   # a line in the kernel trace and in marks.txt, on the same clock
	echo "LAG $*" > $T/trace_marker 2>/dev/null
	echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$OUT/marks.txt"
}

# the rig, with Xorg without glamor for the software baseline
mkdir -p "$OUT/rig"
cp "$RIG/desktop-up.sh" "$RIG/xorg.conf" "$OUT/rig/"
[ $V = software ] && sed -i 's/"AccelMethod" "glamor"/"AccelMethod" "none"/' "$OUT/rig/xorg.conf"
# into the box's /tmp, which desktop-up.sh binds over the root's
cp "$RIG/lag-wl.sh" /tmp/lag-wl.sh
chmod 755 /tmp/lag-wl.sh

XENV="$xenv" XARGS="$xargs" CORES="${CORES:-}" sh "$OUT/rig/desktop-up.sh" "$R" start > "$OUT/up.txt" 2>&1 || { cat "$OUT/up.txt"; exit 1; }
# whatever happens next, the box is left without X, perf or the sampler
perfpid= sampler=
cleanup() {   # (busybox sh exits with the trap's status: keep the script's)
	st=$?
	[ -n "$perfpid" ] && kill -INT $perfpid 2>/dev/null
	[ -n "$sampler" ] && kill $sampler 2>/dev/null
	[ -n "$rsppid" ] && { kill $rsppid 2>/dev/null; for p in $(ps | grep '[r]spcheck' | awk '{print $1}'); do kill $p; done; }
	sleep 2
	sh "$OUT/rig/desktop-up.sh" "$R" stop > /dev/null 2>&1
	rm -f /tmp/lag-perf-*.data
	exit $st
}
trap cleanup EXIT
sleep 30
# RSPCHECK=1: tests/crash/rspcheck (in the box's /tmp) beside the workloads,
# two niced threads checking their own stack pointer
rsppid=
if [ -n "${RSPCHECK:-}" ] && [ -x /tmp/rspcheck ]; then
	chroot "$R" nice -n 10 /tmp/rspcheck 2 100000 > "$OUT/rspcheck.txt" 2>&1 &
	rsppid=$!
fi
# the session's D-Bus, for the clients started here (xfwm4 reads its settings
# through Xfconf, on it)
sess=$(pidof xfce4-session | cut -d' ' -f1)
dbus=$([ -n "$sess" ] && tr '\0' '\n' < /proc/$sess/environ | grep '^DBUS_SESSION_BUS_ADDRESS=')
cenv="$cenv $dbus"
# xfwm4 again, with this variant's Mesa, options and stats
as_desk sh -c "exec xfwm4 --replace $xfwm" > /tmp/lag-xfwm4.err 2>&1 &
sleep 8
as_desk xrandr > "$OUT/xrandr.txt" 2>&1
as_desk timeout 20 glxinfo -B > "$OUT/glxinfo.txt" 2>&1
if [ $V != software ] && ! grep -q 'SGX545' "$OUT/glxinfo.txt"; then
	echo "clients are not on our Mesa (glxinfo: $(grep -m1 'renderer string' "$OUT/glxinfo.txt"))" >> "$OUT/notes.txt"
	echo "lag-box.sh: clients are not on our Mesa, see $OUT/glxinfo.txt" >&2
fi
# whose Mesa each one runs: ours is /opt/sgx/lib, the root's own is not
for p in $(pidof Xorg) $(pidof xfwm4); do
	echo "$p $(tr '\0' ' ' < /proc/$p/cmdline): $(grep -o '/opt/sgx/lib/[^ ]*\|/usr/lib/x86_64-linux-gnu/dri/[^ ]*' /proc/$p/maps | sort -u | tr '\n' ' ')"
done > "$OUT/libs.txt"

# the job trace
echo 0 > $T/tracing_on
echo > $T/trace
echo 16384 > $T/buffer_size_kb
echo mono > $T/trace_clock 2>/dev/null
for e in pvrsgx_kick pvrsgx_job_done pvrsgx_render_kick; do
	{ echo 1 > $T/events/powervr_sgx/$e/enable; } 2>/dev/null || echo "no tracepoint $e" >> "$OUT/notes.txt"
done
# the kick runs in the scheduler's worker: the submitting process comes from
# drm_sched's own events (drm_sched_job in the submitter, drm_run_job in the
# worker just before the kick)
for e in drm_sched_job drm_run_job; do
	{ echo 1 > $T/events/gpu_scheduler/$e/enable; } 2>/dev/null || echo "no tracepoint $e: GPU time by worker, not by process" >> "$OUT/notes.txt"
done
echo 1 > $T/tracing_on

# CPU time per process (clock ticks), every 0.5 s
(
	while :; do
		t=$(cut -d' ' -f1 /proc/uptime)
		echo "T $t $(head -1 /proc/stat)"
		for p in $(pidof Xorg xfwm4 xfce4-terminal glmark2 firefox-esr 2>/dev/null); do
			[ -r /proc/$p/stat ] && echo "P $t $p $(sed 's/.*) //' /proc/$p/stat | cut -d' ' -f12,13) $(cat /proc/$p/comm)"
		done
		sleep 0.5
	done
) > "$OUT/cpu.txt" 2>/dev/null &
sampler=$!
# perf in the root (linux-perf), system-wide while each workload runs: Xorg,
# xfwm4 and the client by name, with our Mesa's symbols from /opt/sgx/lib
PERF=
for p in /usr/bin/perf /usr/sbin/perf; do [ -x "$R$p" ] && PERF="chroot $R $p" && break; done
[ -z "$PERF" ] && command -v perf > /dev/null && PERF=perf
[ -n "${NOPERF:-}" ] && PERF=
[ -z "$PERF" ] && echo "no perf in the root or on the box: no CPU profiles" >> "$OUT/notes.txt"

for w in "$@"; do
	xl0=$(wc -l < "$R/var/log/xorg.out")
	fl0=$(wc -l < /tmp/lag-xfwm4.err)
	perfpid=
	if [ -n "$PERF" ] && [ $w != idle ]; then
		$PERF record -a -g -F 499 -o /tmp/lag-perf-$w.data > /tmp/lag-perf-$w.log 2>&1 &
		perfpid=$!
		sleep 1
	fi
	mark start "$V" "$w"
	as_desk timeout 180 sh /tmp/lag-wl.sh "$w" > "$OUT/wl-$w.txt" 2>&1
	mark end "$V" "$w"
	# what X has, and what the display scans out (simpledrm's boot
	# framebuffer, where /root/m3/memdump is there): is the screen right
	as_desk import -window root -resize 50% "png:/tmp/lag-$V-$w.png" > /dev/null 2>&1 &&
		mv "/tmp/lag-$V-$w.png" "$OUT/x-$w.png" 2>/dev/null
	[ -x /root/m3/memdump ] && [ -n "${SCANOUT:-}" ] &&
		/root/m3/memdump $SCANOUT "${SCANOUT_BYTES:-1413120}" > "$OUT/scanout-$w.raw" 2>/dev/null
	if [ -n "$perfpid" ]; then
		kill -INT $perfpid; wait $perfpid
		# by process, then where each spends it
		$PERF report -i /tmp/lag-perf-$w.data --stdio --no-children --sort comm,dso -g none \
			> "$OUT/perf-$w-dso.txt" 2>/dev/null
		$PERF report -i /tmp/lag-perf-$w.data --stdio --no-children --sort comm,dso,sym -g none \
			--percent-limit 0.5 > "$OUT/perf-$w-sym.txt" 2>/dev/null
		$PERF report -i /tmp/lag-perf-$w.data --stdio --no-children --comms Xorg,xfwm4 \
			-g caller,0.5,callee --percent-limit 2 > "$OUT/perf-$w-calls.txt" 2>/dev/null
		mv /tmp/lag-perf-$w.data "$OUT/perf-$w.data"
		perfpid=
	fi
	sleep 3   # the stats line after the workload (every 2 s)
	sed -n "$((xl0 + 1)),\$p" "$R/var/log/xorg.out" | grep '^sgx stats' > "$OUT/stats-x-$w.txt"
	sed -n "$((fl0 + 1)),\$p" /tmp/lag-xfwm4.err | grep '^sgx stats' > "$OUT/stats-xfwm4-$w.txt"
done

kill $sampler
sampler=
echo 0 > $T/tracing_on
cat $T/trace > "$OUT/trace.txt"
for e in pvrsgx_kick pvrsgx_job_done pvrsgx_render_kick; do
	{ echo 0 > $T/events/powervr_sgx/$e/enable; } 2>/dev/null
done
for e in drm_sched_job drm_run_job; do
	{ echo 0 > $T/events/gpu_scheduler/$e/enable; } 2>/dev/null || true
done
echo 1408 > $T/buffer_size_kb
cp "$R/var/log/Xorg.0.log" "$R/var/log/xorg.out" "$R/var/log/xfce.out" "$OUT/" 2>/dev/null
cp /tmp/lag-xfwm4.err "$OUT/xfwm4.err"
if [ $V = shaderdump ]; then
	mkdir -p "$OUT/shaders"
	cp /tmp/lag-shaders/* "$OUT/shaders/" 2>/dev/null
	rm -rf /tmp/lag-shaders
fi
{ getconf CLK_TCK 2>/dev/null || chroot "$R" getconf CLK_TCK 2>/dev/null || echo 100; } > "$OUT/clk_tck.txt"
dmesg | grep -i 'powervr\|sgx' | tail -40 > "$OUT/dmesg.txt"
echo "lag-box.sh: $V done"
