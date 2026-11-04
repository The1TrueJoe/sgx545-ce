#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# One Minecraft benchmark run on the EA, on the old stack or the new, the same
# way for both:
#
#   old  the openhc-minecraft install as mc-session runs it: i386 Zulu 8, gl4es,
#        the DDK's GLES2 on pvrsrvkm, Xvfb (the normal boot)
#   new  mc-x64.sh: x64 Zulu 8, LWJGL's amd64 natives, GLX/DRI3 on Xorg, our Mesa
#        on powervr_sgx (a RAM boot, x11-up.sh's root at /opt/x64root)
#
#   mc-oldnew.sh old|new RUN [WARM] [STAND] [WALK]       (seconds: 30 60 60)
#
# Both: vanilla 1.7.10 (no OptiFine, no tweaker), 720x480, -Xmx768M, this
# script's options.txt, a fresh copy of the A/B rig's benchmark world in a game
# directory of its own (the player's saves are never touched), the same clicks
# into it. After the world is joined: WARM s, then STAND s looking where the
# world's spawn looks, then WALK s holding W.
#
# FPS: frames presented per second, as each stack counts its swaps: gl4es'
# LIBGL_FPS line on the old (MC_LINEBUF=1: unbuffered), the driver's
# SGX545_DEBUG=stats line on the new; the mean of the intervals inside a
# window. CPU: the java process's user+system time over the window. GPU
# recoveries: the kernel log's count over the whole run (pvrsrvkm's
# "HWRecoveryResetSGX"/"Recovery triggered", powervr_sgx's "job timed out");
# any recovery voids the run. A screenshot with F3's overlay at the end of
# the standing view: /root/mcbench/<who>-<run>.raw (BGRX, see the .txt).
# Results go to /root/mcbench/<who>-<run>.txt.
set -u
WHO=$1 RUN=$2 WARM=${3:-30} STAND=${4:-60} WALK=${5:-60}
OUT=/root/mcbench
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
case $WHO in
old)
	R=/opt/minecraft/rootfs GIN=/opt/mc-oldnew G=$R$GIN LOG=$G/run.log
	REC="HWRecoveryResetSGX|Recovery triggered"
	;;
new)
	R=/opt/x64root GIN=/opt/mc-x64 G=$R$GIN LOG=$G/logs/oldnew.log
	REC="job timed out"
	;;
*) echo "old or new" >&2; exit 2 ;;
esac
I=/opt/minecraft/rootfs/opt

# the game directory: read-only links to the install (old) or the staged copy (new)
mkdir -p "$G/logs"
if [ $WHO = old ]; then
	for d in libraries versions assets natives 1.7.10.launch 1.7.10.files; do
		[ -e "$G/$d" ] || ln -s /opt/minecraft/$d "$G/$d"
	done
fi
rm -rf "$G/saves" && mkdir -p "$G/saves"
cp -a "$I/mc-ab/bench-world" "$G/saves/bench"
rm -f "$G/optionsof.txt"
cat > "$G/options.txt" <<'OPT'
renderDistance:4
fancyGraphics:false
ao:0
clouds:false
particles:0
enableVsync:false
maxFps:260
mipmapLevels:0
anaglyph3d:false
viewBobbing:true
guiScale:2
fov:0.0
gamma:0.0
pauseOnLostFocus:false
snooperEnabled:false
showInventoryAchievementHint:false
lastServer:
OPT

exec 9>/tmp/gpu.lock
flock -n 9 || { echo "mc-oldnew: /tmp/gpu.lock is held" >&2; exit 1; }
rec0=$(dmesg | grep -c -E "$REC")

# the X server and the game
mounted=
if [ $WHO = old ]; then
	for m in dev proc sys tmp; do
		mountpoint -q $R/$m && continue
		mounted="$m $mounted"
		case $m in proc) mount -t proc proc $R/proc ;; sys) mount -t sysfs sys $R/sys ;; *) mount --bind /$m $R/$m ;; esac
	done
	mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
	rm -f /tmp/.X0-lock /tmp/.X11-unix/X0
	chroot $R /usr/bin/env -i PATH=/usr/bin:/bin /usr/bin/Xvfb :0 -screen 0 720x480x24 -nolisten tcp \
		> /tmp/mc-oldnew-xvfb.log 2>&1 &
	i=0; while [ ! -S /tmp/.X11-unix/X0 ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
	X() { chroot $R /usr/bin/env -i DISPLAY=:0 PATH=/usr/bin:/bin xdotool "$@"; }
	chroot $R /usr/bin/env -i HOME=/root TERM=linux DISPLAY=:0 PATH=/usr/local/bin:/usr/bin:/bin \
		MC_GL=ddk MC_DIR=$GIN MC_TWEAKS=0 MC_WIDTH=720 MC_HEIGHT=480 LIBGL_FPS=1 MC_LINEBUF=1 \
		LD_PRELOAD=/usr/lib/sgx/sgx-busidshim.so /usr/local/bin/minecraft-ab Player > "$LOG" 2>&1 &
	READY="foliage.png|textures/items-atlas"
	fps() { grep -o 'fps: [0-9.]*' | awk '{print $2}'; }
else
	[ -S /tmp/.X11-unix/X0 ] || "$here/x11-up.sh" $R start
	X() { "$here/x11-up.sh" $R run xdotool "$@"; }
	"$here/x11-up.sh" $R run env -u MESA_GL_VERSION_OVERRIDE -u MESA_GLSL_VERSION_OVERRIDE \
		SGX545_DEBUG=stats${MC_DEBUG:+,$MC_DEBUG} MC_OPTIFINE=0 MC_TWEAKS=0 MC_DIR=$GIN $GIN/mc-x64.sh Player > "$LOG" 2>&1 &
	READY="textures/items-atlas"
	fps() { grep -o 'sgx545 stats [0-9.]* s: [0-9.]* FPS' | awk '{print $5}'; }
fi
jpid() { ps | grep '[b]in/java' | awk '{print $1}' | head -1; }
ticks() { awk '{print $14 + $15}' /proc/$1/stat 2>/dev/null || echo 0; }
mean() { awk '{s += $1; n++} END {if (n) printf "%.1f", s / n; else printf "-"}'; }

# menu, then the world
w=0; while ! grep -q -E "$READY" "$LOG" 2>/dev/null && [ $w -lt 300 ]; do sleep 1; w=$((w+1)); done
# LWJGL samples the mouse once a frame: a press and release need time between them
click() { X mousemove "$1" "$2"; sleep 1; X mousedown 1; sleep 1; X mouseup 1; sleep 2; }
sleep 10
click 360 236     # Singleplayer
click 360 205     # the world
click 202 396     # Play Selected World
w=0; while ! grep -q 'joined the game' "$LOG" 2>/dev/null && [ $w -lt 300 ]; do sleep 1; w=$((w+1)); done
sleep "$WARM"

window() {   # SECS: FPS mean and java CPU % over the next SECS seconds
	l0=$(wc -l < "$LOG"); p=$(jpid); c0=$(ticks $p)
	sleep "$1"
	c1=$(ticks $p)
	f=$(tail -n +$((l0 + 1)) "$LOG" | fps | mean)
	echo "$f fps, java CPU $(( (c1 - c0) * 100 / (100 * $1) ))%"
}
# MC_DEBUG=flags (new stack): more SGX545_DEBUG flags, e.g. nir (the game's log keeps them)
# MC_PROFILE=SECS (new stack): perf record of java for SECS of the standing view,
# its report in /root/mcbench/new-<run>.perf.txt
if [ -n "${MC_PROFILE:-}" ] && [ $WHO = new ]; then
	chroot $R /usr/bin/perf record -q -F 499 -g -p "$(jpid)" -o /tmp/perf-mc.data -- sleep "$MC_PROFILE" \
		> /dev/null 2>&1 &
fi
stand=$(window "$STAND")
if [ -n "${MC_PROFILE:-}" ] && [ $WHO = new ]; then
	chroot $R /usr/bin/perf report -i /tmp/perf-mc.data --stdio --no-children --sort dso,symbol \
		--percent-limit 0.5 2>/dev/null | grep -v '^$' | head -80 > "$OUT/$WHO-$RUN.perf.txt"
fi
X key F3
sleep 3
if [ $WHO = old ]; then
	cat /dev/fb0 > "$OUT/$WHO-$RUN.raw"
	geo="fb0 $(cat /sys/class/graphics/fb0/virtual_size) stride $(cat /sys/class/graphics/fb0/stride) bpp $(cat /sys/class/graphics/fb0/bits_per_pixel)"
else
	"$here/memdump" 0x7fc00000 $((736 * 480 * 4)) > "$OUT/$WHO-$RUN.raw"
	geo="simpledrm scanout 720x480 stride 2944 bpp 32"
fi
X key F3
X keydown w
walk=$(window "$WALK")
X keyup w
rec=$(( $(dmesg | grep -c -E "$REC") - rec0 ))

for p in $(ps | grep '[b]in/java' | awk '{print $1}'); do kill $p; done
sleep 5
for p in $(ps | grep '[b]in/java' | awk '{print $1}'); do kill -9 $p; done
if [ $WHO = old ]; then
	for p in $(ps | grep '[X]vfb :0' | awk '{print $1}'); do kill $p; done
	sleep 2
	for m in $mounted; do umount $R/$m; done   # only what this run mounted
else
	"$here/x11-up.sh" $R stop
fi
{
	echo "who $WHO run $RUN warm $WARM stand $STAND walk $WALK"
	echo "standing: $stand"
	echo "walking: $walk"
	echo "gpu recoveries: $rec"
	echo "screenshot: $geo"
} | tee "$OUT/$WHO-$RUN.txt"
