#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# The performance suite, on the EA, for one stack (docs/PERF.md):
#
#   old  the openhc-minecraft install's stack: gl4es on the DDK's GLES2 (pvrsrvkm,
#        the normal boot), Xvfb; the DDK's own EGL for the microbenchmark
#   new  our Mesa on powervr_sgx (a RAM boot): GLX/DRI3 on Xorg in /opt/x64root,
#        EGL surfaceless for the microbenchmark
#
#   run-suite.sh old|new [RUNS]          (RUNS: 3)
#
# Per run: Minecraft standing and walking (mc-oldnew.sh); glmark2's default
# scenes in a 720x480 window and off-screen (desktop GL, through gl4es on the
# old stack); the GPU microbenchmark (sgx545_perfmicro). The new stack adds
# glmark2-es2 and glmark2-es2-drm (straight to the display, Xorg stopped).
# Any GPU recovery voids that run. Results: /root/perf/<stack>/, a summary
# with each measure's median, min and max in summary.txt.
#
# Needs (staged by the host): /root/m3 (mc-oldnew.sh, x11-up.sh, memdump),
# /root/perf/bin (sgx545_perfmicro-i386 and -x86_64), the old stack's glmark2
# in /opt/minecraft/rootfs/opt/perf-old (its own directory), the new stack's in
# /opt/x64root, our x86_64 Mesa in /root/deqp/lib-m3c.
set -u
WHO=$1 RUNS=${2:-3}
O=/root/perf/$WHO
B=/root/perf/bin
M=/root/m3
R_OLD=/opt/minecraft/rootfs
R_NEW=/opt/x64root
mkdir -p "$O"
: > "$O/results.tsv"
rec() { case $WHO in old) dmesg | grep -c -E 'HWRecoveryResetSGX|Recovery triggered' ;; new) dmesg | grep -c 'job timed out' ;; esac; }
put() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$O/results.tsv"; }   # measure, run, value ("void" when a run recovered)

xvfb_up() {
	for m in dev proc sys tmp; do
		mountpoint -q $R_OLD/$m && continue
		XVFB_MOUNTED="$m ${XVFB_MOUNTED:-}"
		case $m in proc) mount -t proc proc $R_OLD/proc ;; sys) mount -t sysfs sys $R_OLD/sys ;; *) mount --bind /$m $R_OLD/$m ;; esac
	done
	mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
	rm -f /tmp/.X0-lock /tmp/.X11-unix/X0
	chroot $R_OLD /usr/bin/env -i PATH=/usr/bin:/bin /usr/bin/Xvfb :0 -screen 0 720x480x24 -nolisten tcp > /tmp/perf-xvfb.log 2>&1 &
	i=0; while [ ! -S /tmp/.X11-unix/X0 ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
}
xvfb_down() {
	for p in $(ps | grep '[X]vfb :0' | awk '{print $1}'); do kill $p; done
	sleep 2
	for m in ${XVFB_MOUNTED:-}; do umount $R_OLD/$m; done
	XVFB_MOUNTED=
}

glmark() {   # NAME ARGS...: glmark2's score, the run voided by a recovery
	name=$1; shift
	r0=$(rec)
	if [ $WHO = old ]; then
		D=/opt/perf-old/root
		out=$(chroot $R_OLD /usr/bin/env -i HOME=/root DISPLAY=:0 PATH=/usr/local/bin:/usr/bin:/bin \
			LD_LIBRARY_PATH=$D/usr/lib/i386-linux-gnu:$D/lib/i386-linux-gnu \
			/usr/local/bin/gl4es-run-ddk $D/usr/bin/glmark2 --data-path $D/usr/share/glmark2 "$@" 2>&1)
	else
		out=$($M/x11-up.sh $R_NEW run env -u MESA_GL_VERSION_OVERRIDE -u MESA_GLSL_VERSION_OVERRIDE "$@" 2>&1)
	fi
	s=$(echo "$out" | grep -o 'glmark2 Score: [0-9]*' | awk '{print $3}')
	echo "$out" > "$O/$name-$run.log"
	[ "$(rec)" = "$r0" ] || s=void
	put "$name" $run "${s:-fail}"
}

exec 9>/tmp/gpu.lock
flock -n 9 || { echo "run-suite: /tmp/gpu.lock is held" >&2; exit 1; }
for run in $(seq 1 $RUNS); do
	# Minecraft (mc-oldnew.sh takes the GPU lock itself: let it go for the call)
	flock -u 9
	$M/mc-oldnew.sh $WHO $run > /dev/null 2>&1
	flock -n 9 || { echo "run-suite: lost /tmp/gpu.lock" >&2; exit 1; }
	f=/root/mcbench/$WHO-$run.txt
	v=$(grep -c 'gpu recoveries: 0' $f 2>/dev/null)
	put minecraft-standing $run "$([ "$v" = 1 ] && awk '/standing/{print $2}' $f || echo void)"
	put minecraft-walking $run "$([ "$v" = 1 ] && awk '/walking/{print $2}' $f || echo void)"
	put minecraft-java-cpu-standing $run "$(awk -F'CPU ' '/standing/{print $2}' $f)"

	# glmark2
	if [ $WHO = old ]; then
		xvfb_up
		glmark glmark2-window -s 720x480
		glmark glmark2-offscreen -s 720x480 --off-screen
		xvfb_down
	else
		$M/x11-up.sh $R_NEW start > /dev/null
		glmark glmark2-window glmark2 -s 720x480
		glmark glmark2-offscreen glmark2 -s 720x480 --off-screen
		glmark glmark2-es2-window glmark2-es2 -s 720x480
		$M/x11-up.sh $R_NEW stop
		r0=$(rec)
		out=$(chroot $R_NEW /usr/bin/env LD_LIBRARY_PATH=/opt/sgx545/lib GBM_BACKENDS_PATH=/opt/sgx545/lib/gbm \
			__EGL_VENDOR_LIBRARY_DIRS=/opt/sgx545/share/glvnd/egl_vendor.d glmark2-es2-drm -s 720x480 2>&1)
		echo "$out" > "$O/glmark2-es2-drm-$run.log"
		s=$(echo "$out" | grep -o 'glmark2 Score: [0-9]*' | awk '{print $3}')
		[ "$(rec)" = "$r0" ] || s=void
		put glmark2-es2-drm $run "${s:-fail}"
	fi

	# the GPU microbenchmark
	r0=$(rec)
	if [ $WHO = old ]; then
		out=$(LD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so /opt/sgx-i386/lib/ld-linux.so.2 \
			--library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib $B/sgx545_perfmicro-i386 2>&1)
	else
		out=$(LD_LIBRARY_PATH=/root/deqp/lib-m3c EGL_PLATFORM=surfaceless $B/sgx545_perfmicro-x86_64 2>&1)
	fi
	echo "$out" > "$O/micro-$run.log"
	ok=$([ "$(rec)" = "$r0" ] && echo 1)
	for t in fill fill-blend tex verts; do
		v=$(echo "$out" | awk -v t="$t:" '$1 == t {print $4}')
		put "micro-$t" $run "$([ -n "$ok" ] && echo "${v:-fail}" || echo void)"
	done
done

# median (min-max) per measure, over the runs that count
{
	echo "stack $WHO, $(uname -r), $(date -u +%FT%TZ), $RUNS runs"
	cut -f1 "$O/results.tsv" | awk '!seen[$0]++' | while read -r m; do
		vals=$(awk -F'\t' -v m="$m" '$1 == m && $3 ~ /^[0-9.]+%?$/ {sub(/%/, "", $3); print $3}' "$O/results.tsv" | sort -n)
		n=$(echo "$vals" | grep -c .)
		bad=$(awk -F'\t' -v m="$m" '$1 == m && $3 !~ /^[0-9.]+%?$/' "$O/results.tsv" | wc -l)
		if [ "$n" -gt 0 ]; then
			med=$(echo "$vals" | awk '{a[NR] = $1} END {print (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2}')
			printf '%s\t%s\t(%s-%s)\t%s runs%s\n' "$m" "$med" "$(echo "$vals" | head -1)" "$(echo "$vals" | tail -1)" \
				"$n" "$([ "$bad" -gt 0 ] && echo ", $bad void or failed")"
		else
			printf '%s\t-\t\t0 runs, %s void or failed\n' "$m" "$bad"
		fi
	done
} | tee "$O/summary.txt"
