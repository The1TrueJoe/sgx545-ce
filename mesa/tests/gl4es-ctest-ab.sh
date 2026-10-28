#!/bin/sh
# gl4es' ctest traces on the EA3, through gl4es, on the DDK and on ours.
#
#   gl4es-ctest-ab.sh [ddk|ours|both] [TEST...]
#
# Runs on the box (BusyBox sh) against the openhc-minecraft runtime at
# /opt/minecraft/rootfs. Needs, inside the root:
#   /usr/lib/gl4es/libGL.so.1     the runtime's gl4es (unmodified)
#   /usr/lib/sgx/                 the DDK (mc-session's ddk_sync puts it there)
#   /usr/lib/sgx545/              our Mesa build (mesa/build.sh i386)
#   /usr/bin/glretrace, Xvfb      apitrace's replayer and the X server
#   /ctest/traces, /ctest/refs    gl4es' tests/../traces and refs
# Each test replays its trace with gl4es in pbuffer mode (LIBGL_FB=3, as
# gl4es' tests/test.sh) and snapshots the call ctest checks, into
# /ctest/out/<who>-<test>-es<N>-<call>.png. tools: mesa/tests/ctest_cmp.py
# compares them with each other and with gl4es' references.
#
# One GL client at a time: every replay holds the box's GPU lock.
# CTEST_ENV="VAR=value ..." is added to each replay's environment (debugging).
# CTEST_FB is gl4es' LIBGL_FB: 3 (pbuffer, as gl4es' own tests; the default) or
# 1 (fb0 window, as the game runs; the DDK's pbuffers crash in
# SGXAddRenderTarget under gl4es on the EA3).
R=/opt/minecraft/rootfs
WHO=${1:-both}; [ $# -gt 0 ] && shift
# name call es: gl4es' CMakeLists create_test()/create_test_GLES() list
ALL="glxgears:0000008203:1 glxgears:0000008203:2
stuntcarracer:0000118817:1 stuntcarracer:0000118817:2
neverball:0000078750:1 neverball:0000078750:2
foobillardplus:0000014748:1 foobillardplus:0000014748:2
descent3:0000284192:1 descent3:0000284192:2
pointsprite:0000248810:1 pointsprite:0000248810:2
openra:0000031249:2 glsl_lighting:0000505393:2"

for m in dev proc sys tmp; do
	mountpoint -q $R/$m && continue
	case $m in
		proc) mount -t proc proc $R/proc ;;
		sys) mount -t sysfs sys $R/sys ;;
		*) mount --bind /$m $R/$m ;;
	esac
done
mkdir -p $R/ctest/out /tmp/.X11-unix

inchroot() {
	chroot $R /usr/bin/env -i HOME=/root PATH=/usr/local/bin:/usr/bin:/bin DISPLAY=:1 "$@"
}

if [ ! -S /tmp/.X11-unix/X1 ]; then
	inchroot /usr/bin/Xvfb :1 -screen 0 1024x768x24 -nolisten tcp > /tmp/ctest-xvfb.log 2>&1 &
	i=0; while [ ! -S /tmp/.X11-unix/X1 ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
fi

replay() {	# who name call es
	who=$1 name=$2 call=$3 es=$4
	case $who in
	ddk)	lib=/usr/lib/sgx pre=/usr/lib/sgx/sgx-busidshim.so
		gles=$lib/libGLESv2.so.2; [ $es = 1 ] && gles=$lib/libGLES_CM.so.1 ;;
	ours)	lib=/usr/lib/sgx545 pre=
		gles=$lib/libGLESv2.so.2; [ $es = 1 ] && gles=$lib/libGLESv1_CM.so.1 ;;
	esac
	[ -f $R/ctest/$name.trace ] || (cd $R/ctest && gunzip -c traces/$name.tgz | tar xf -)
	out=/ctest/out/$who-$name-es$es-
	rm -f $R$out*
	start=$(cut -d' ' -f1 /proc/uptime)
	flock /tmp/gpu.lock chroot $R /usr/bin/env -i HOME=/root PATH=/usr/local/bin:/usr/bin:/bin \
		DISPLAY=:1 LIBGL_FB=${CTEST_FB:-3} LIBGL_ES=$es LIBGL_SILENTSTUB=1 LIBGL_NOBANNER=1 LIBGL_NOERROR=1 \
		LIBGL_DEEPBIND=0 LIBGL_EGL=$lib/libEGL.so.1 LIBGL_GLES=$gles \
		LD_LIBRARY_PATH=/usr/lib/gl4es:$lib ${pre:+LD_PRELOAD=$pre} \
		$CTEST_ENV glretrace --singlethread -s $out --snapshot=$call /ctest/$name.trace > $R/ctest/out/$who-$name-es$es.log 2>&1
	rc=$?
	end=$(cut -d' ' -f1 /proc/uptime)
	png=$(ls $R$out*.png 2>/dev/null | head -n 1)
	echo "$who $name es$es rc=$rc $(awk "BEGIN{printf \"%.1f s\", $end-$start}") ${png:+snapshot}"
}

# SGX recoveries since uptime $1 (dmesg rolls over, so count by timestamp)
recoveries() {
	dmesg | grep 'Recovery triggered' | awk -F'[][]' -v t="$1" '$2 + 0 > t { n++ } END { print n + 0 }'
}
for t in $ALL; do
	name=${t%%:*} rest=${t#*:} call=${rest%%:*} es=${rest#*:}
	if [ $# -gt 0 ]; then
		case " $* " in *" $name "*|*" $name:$es "*) ;; *) continue ;; esac
	fi
	for who in ddk ours; do
		case $WHO in both|$who) ;; *) continue ;; esac
		t0=$(cut -d' ' -f1 /proc/uptime)
		replay $who $name $call $es
		n=$(recoveries $t0)
		[ "$n" != 0 ] && echo "!! $n SGX recoveries in $who $name es$es"
	done
done
