#!/bin/sh
# Minecraft on the EA3 through gl4es, on the DDK or on ours, for the A/B.
#   mc-ab.sh ddk|sgx545 SECONDS [xdotool steps: "at:sec cmd args" ...]
# Runs the openhc-minecraft runtime's game (/opt/minecraft/rootfs) on Xvfb :0
# with gl4es' FPS counter, under the GPU lock and gpuwatch.sh; after SECONDS
# the panel (fb0) is saved to /opt/minecraft/rootfs/ctest/out/mc-<who>.fb
# (MC_BURST=n: and n more, mc-<who>-<k>.fb) and the game stopped. xdotool steps (sent through Xvfb) drive the menus: at
# second `at`, run `xdotool cmd args`. MC_DIR (in MC_ENV): another game
# directory in the root, so a test world leaves the player's saves alone.
R=/opt/minecraft/rootfs
WHO=$1 SECS=$2; shift 2
D=$(dirname "$0")
for m in dev proc sys tmp; do
	mountpoint -q $R/$m && continue
	case $m in proc) mount -t proc proc $R/proc ;; sys) mount -t sysfs sys $R/sys ;; *) mount --bind /$m $R/$m ;; esac
done
# the DDK's per-executable hints, as mc-session writes them
cat > $R/etc/powervr.ini <<'INI'
[java]
OverloadTexLayout=1
DisableHWTextureUpload=1
DefaultVertexBufferSize=2097152
MaxVertexBufferSize=4194304
DefaultIndexBufferSize=1048576
DefaultPDSVertBufferSize=262144
DefaultVDMBufferSize=131072
INI
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
rm -f /tmp/.X0-lock /tmp/.X11-unix/X0
chroot $R /usr/bin/env -i PATH=/usr/bin:/bin /usr/bin/Xvfb :0 -screen 0 720x480x24 -nolisten tcp -a 1 -t 0 > /tmp/mc-xvfb.log 2>&1 &
xvfb=$!
i=0; while [ ! -S /tmp/.X11-unix/X0 ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
pre=; [ "$WHO" = ddk ] && pre=LD_PRELOAD=/usr/lib/sgx/sgx-busidshim.so
$D/gpuwatch.sh chroot $R /usr/bin/env -i HOME=/root TERM=linux DISPLAY=:0 PATH=/usr/local/bin:/usr/bin:/bin \
	MC_GL=$WHO MC_WIDTH=720 MC_HEIGHT=480 LIBGL_FPS=1 $MC_ENV $pre \
	/usr/local/bin/minecraft-ab Player > $R/ctest/out/mc-$WHO.log 2>&1 &
game=$!
# MC_READY=regex: the clock (SECONDS and the steps) starts when the game's
# log first matches it, as loading times differ by stack and box
if [ -n "$MC_READY" ]; then
	w=0
	while ! grep -q -E "$MC_READY" $R/ctest/out/mc-$WHO.log 2>/dev/null && kill -0 $game 2>/dev/null && [ $w -lt 900 ]; do
		sleep 1; w=$((w+1))
	done
	echo "mc-ab: ready after $w s"
fi
t=0
while [ $t -lt $SECS ] && kill -0 $game 2>/dev/null; do
	for step in "$@"; do
		at=${step%%:*} cmd=${step#*:}
		[ "$at" = "$t" ] && chroot $R /usr/bin/env -i DISPLAY=:0 PATH=/usr/bin:/bin xdotool $cmd
	done
	sleep 1; t=$((t+1))
done
cat /dev/fb0 > $R/ctest/out/mc-$WHO.fb
# MC_BURST=n: n more panel dumps, 0.3 s apart (ours draws into fb0 itself,
# single-buffered, so one dump can catch a frame half drawn)
k=0
while [ $k -lt ${MC_BURST:-0} ]; do
	cat /dev/fb0 > $R/ctest/out/mc-$WHO-$k.fb
	# with gl4es drawing into the X window (MC_FB=3), the picture is there
	[ -x $R/usr/bin/xwd ] && chroot $R /usr/bin/env -i DISPLAY=:0 PATH=/usr/bin:/bin xwd -root -silent > $R/ctest/out/mc-$WHO-$k.xwd 2>/dev/null
	sleep 0.3; k=$((k+1))
done
killall java 2>/dev/null; sleep 3; killall -9 java 2>/dev/null
wait $game 2>/dev/null
kill $xvfb 2>/dev/null
echo "mc-ab: $WHO ran $t s; $(grep -c . $R/ctest/out/mc-$WHO.log) log lines; FPS: $(grep -o "FPS: [0-9.]*" $R/ctest/out/mc-$WHO.log | tail -5 | tr '\n' ' ')"
