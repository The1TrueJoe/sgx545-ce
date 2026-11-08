#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On the EA (normal boot, the old stack): makes the benchmark world that
# mc-oldnew.sh copies before every run, when the box has none
# (/opt/minecraft/rootfs/opt/mc-ab/bench-world). A new world with the game's
# random seed, in a game directory of its own; after it is joined, two
# autosaves (45 s apart), then the game is stopped. Keep a copy off the box
# and note the seed: a reverted box loses it.
#   mc-benchworld.sh [SECS after joining]   (default 100)
set -u
R=/opt/minecraft/rootfs GIN=/opt/mc-gen G=$R$GIN A=$R/opt/mc-ab
SECS=${1:-100}
[ -d "$A/bench-world" ] && { echo "mc-benchworld: $A/bench-world exists" >&2; exit 1; }
mkdir -p "$G/logs" "$A"
for d in libraries versions assets natives 1.7.10.launch 1.7.10.files; do
	[ -e "$G/$d" ] || ln -s /opt/minecraft/$d "$G/$d"
done
rm -rf "$G/saves"
printf 'renderDistance:4\nfancyGraphics:false\nenableVsync:false\nguiScale:2\npauseOnLostFocus:false\nsnooperEnabled:false\n' > "$G/options.txt"
exec 9>/tmp/gpu.lock
flock -n 9 || { echo "mc-benchworld: /tmp/gpu.lock is held" >&2; exit 1; }
mounted=
for m in dev proc sys tmp; do
	mountpoint -q $R/$m && continue
	mounted="$m $mounted"
	case $m in proc) mount -t proc proc $R/proc ;; sys) mount -t sysfs sys $R/sys ;; *) mount --bind /$m $R/$m ;; esac
done
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
rm -f /tmp/.X0-lock /tmp/.X11-unix/X0
chroot $R /usr/bin/env -i PATH=/usr/bin:/bin /usr/bin/Xvfb :0 -screen 0 720x480x24 -nolisten tcp > /tmp/mc-gen-xvfb.log 2>&1 &
i=0; while [ ! -S /tmp/.X11-unix/X0 ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
LOG=$G/run.log
chroot $R /usr/bin/env -i HOME=/root TERM=linux DISPLAY=:0 PATH=/usr/local/bin:/usr/bin:/bin \
	MC_GL=ddk MC_DIR=$GIN MC_TWEAKS=0 MC_WIDTH=720 MC_HEIGHT=480 \
	LD_PRELOAD=/usr/lib/sgx/sgx-busidshim.so /usr/local/bin/minecraft-ab Player > "$LOG" 2>&1 &
X64=/opt/x64root
X() { env -i DISPLAY=:0 $X64/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 \
	--library-path $X64/usr/lib/x86_64-linux-gnu:$X64/lib/x86_64-linux-gnu $X64/usr/bin/xdotool "$@"; }
click() { X mousemove "$1" "$2"; sleep 1; X mousedown 1; sleep 1; X mouseup 1; sleep 3; }
w=0; while ! grep -q -E "foliage.png|textures/items-atlas" "$LOG" 2>/dev/null && [ $w -lt 300 ]; do sleep 1; w=$((w+1)); done
sleep 10
click 360 236     # Singleplayer
click 518 396     # Create New World
click 200 444     # Create New World (the button on the next screen)
w=0; while ! grep -q 'joined the game' "$LOG" 2>/dev/null && [ $w -lt 400 ]; do sleep 1; w=$((w+1)); done
grep -q 'joined the game' "$LOG" && sleep "$SECS"
for p in $(ps | grep -e '[b]in/java' -e '[X]vfb :0' | awk '{print $1}'); do kill $p; done
sleep 8
for m in $mounted; do umount $R/$m; done
W=$(ls -d "$G/saves"/*/ 2>/dev/null | head -1)
[ -n "$W" ] && [ -f "$W/level.dat" ] || { echo "mc-benchworld: no world was saved; see $LOG" >&2; exit 1; }
cp -a "$W" "$A/bench-world"
du -sh "$A/bench-world"; grep -c 'joined the game' "$LOG"
echo "mc-benchworld: $A/bench-world from $(basename "$W")"
