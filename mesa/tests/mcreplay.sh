#!/bin/sh
# Replay a recorded Minecraft GL trace on the EA3 through gl4es, on the DDK
# or ours: the A/B of the game's own frames, frame for frame.
#   mcreplay.sh ddk|ours        (INTERVAL=n: a snapshot every n frames, 40)
# The trace (/opt/minecraft/rootfs/ctest/out/mc-world.trace) is recorded with
# mc-ab.sh through apitrace's GLX wrapper: in the root, /usr/lib/mc-trace/
# libGL.so.1 -> apitrace/wrappers/glxtrace.so, a gl4es-run-ddktrace with
# /usr/lib/mc-trace first in LD_LIBRARY_PATH, and
#   MC_ENV="LIBGL_NOTEST=1 MC_TWEAKS=0 MC_LIBGL=/usr/lib/mc-trace/libGL.so.1
#           TRACE_LIBGL=/usr/lib/gl4es/libGL.so.1 TRACE_FILE=/ctest/out/mc-world.trace"
#   mc-ab.sh ddktrace SECONDS <xdotool steps into a world and out again>
# (leave the world through its menu: killing the JVM loses the unflushed end).
R=/opt/minecraft/rootfs; who=$1
for m in dev proc sys tmp; do mountpoint -q $R/$m || case $m in proc) mount -t proc proc $R/proc;; sys) mount -t sysfs sys $R/sys;; *) mount --bind /$m $R/$m;; esac; done
mkdir -p /tmp/.X11-unix
[ -S /tmp/.X11-unix/X1 ] || { chroot $R /usr/bin/env -i PATH=/usr/bin:/bin /usr/bin/Xvfb :1 -screen 0 1024x768x24 -nolisten tcp >/tmp/x1.log 2>&1 & sleep 3; }
case $who in ddk) lib=/usr/lib/sgx pre=LD_PRELOAD=/usr/lib/sgx/sgx-busidshim.so ;; *) lib=/usr/lib/sgx545 pre= ;; esac
rm -f $R/ctest/out/mcw-$who-*.png
/opt/minecraft/gpuwatch.sh chroot $R /usr/bin/env -i HOME=/root PATH=/usr/local/bin:/usr/bin:/bin DISPLAY=:1 \
	LIBGL_FB=3 LIBGL_ES=2 LIBGL_GL=21 LIBGL_NOTEST=1 LIBGL_SILENTSTUB=1 LIBGL_NOBANNER=1 LIBGL_NOERROR=1 LIBGL_DEEPBIND=0 \
	LIBGL_VERTEXFOG=1 LIBGL_MIPMAP=3 LIBGL_EGL=$lib/libEGL.so.1 LIBGL_GLES=$lib/libGLESv2.so.2 \
	LD_LIBRARY_PATH=/usr/lib/gl4es:$lib $pre $REPLAY_ENV \
	glretrace --singlethread -s /ctest/out/mcw-$who- -S '*/frame' --snapshot-interval=${INTERVAL:-40} /ctest/out/mc-world.trace > $R/ctest/out/mcw-$who.log 2>&1
echo "$who: $(ls $R/ctest/out/mcw-$who-*.png 2>/dev/null | wc -l) snapshots; $(tail -1 $R/ctest/out/mcw-$who.log)"
