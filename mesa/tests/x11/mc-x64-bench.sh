#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On the EA (powervr_sgx up, the Debian root staged by mc-x64-stage.sh): one
# run of the A/B rig's benchmark (mcbench.sh) on the 64-bit GLX path: the
# fixed world restored, the same clicks, SECS seconds in it, then the
# driver's FPS (SGX545_DEBUG=stats, every 2 s) and the GPU's job timeouts.
#
#   mc-x64-bench.sh ROOT [SECS]
#
# Starts Xorg if it isn't up and stops the game and Xorg afterwards. Holds
# /tmp/gpu.lock for the run.
set -eu
R=$1 SECS=${2:-170}
here=$(cd "$(dirname "$0")" && pwd)
G=$R/opt/mc-x64
L=$G/logs/bench.log
exec 9>/tmp/gpu.lock
flock -n 9 || { echo "mc-x64-bench: /tmp/gpu.lock is held" >&2; exit 1; }
X() { "$here/x11-up.sh" "$R" run xdotool "$@"; }
stop_game() { for p in $(ps | grep '[j]re8-x64/bin/java' | awk '{print $1}'); do kill "$p"; done; }

[ -S /tmp/.X11-unix/X0 ] || "$here/x11-up.sh" "$R" start
rm -rf "$G/saves" && mkdir -p "$G/saves" "$G/logs"
cp -a "$G/bench-world" "$G/saves/bench"
t0=$(dmesg | grep -c 'job timed out' || true)
"$here/x11-up.sh" "$R" run env -u MESA_GL_VERSION_OVERRIDE -u MESA_GLSL_VERSION_OVERRIDE \
	SGX545_DEBUG=stats /opt/mc-x64/mc-x64.sh > "$L" 2>&1 &
# the menu: the item atlas is the last texture the title screen loads
for i in $(seq 1 120); do grep -q 'textures/items-atlas' "$L" && break; sleep 1; done
sleep 10
X mousemove 360 236 click 1          # Singleplayer
sleep 3
X mousemove 360 205 mousedown 1 mouseup 1   # the world
sleep 3
X mousemove 202 396 click 1          # Play Selected World
sleep "$SECS"
echo "FPS, last 12 two-second windows: $(grep -o 'sgx545 stats [0-9.]* s: [0-9.]* FPS' "$L" | tail -12 | awk '{print $5}' | tr '\n' ' ')"
grep 'sgx545 stats' "$L" | tail -4
echo "job timeouts during the run: $(( $(dmesg | grep -c 'job timed out' || true) - t0 ))"
stop_game
sleep 3
"$here/x11-up.sh" "$R" stop
