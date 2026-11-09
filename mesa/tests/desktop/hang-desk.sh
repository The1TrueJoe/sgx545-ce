#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On a CE5300 box, after smoke.sh's root and mesa-sgx are in place: the desktop
# up, a forced GPU hang from another client (powervr_sgx_hang), then whether the
# desktop and a new GL client still work. Results in /tmp/hangdesk.
#
#   hang-desk.sh ROOT HANGTEST
set -u
R=$1 H=$2
O=/tmp/hangdesk
rm -rf "$O"; mkdir -p -m 1777 "$O"
up=$(dirname "$0")/desktop-up.sh
run() { sh "$up" "$R" run "$@"; }
sh "$up" "$R" start
sleep 25
run import -window root -resize 50% "png:$O/1-before.png"
D0=$(dmesg | wc -l)
"$H" -n 1 > "$O/hang.txt" 2>&1
echo "hang test: $(tail -1 "$O/hang.txt")"
sleep 3
run sh -c "timeout 10 stdbuf -oL es2gears_x11 > $O/gears.txt 2>&1" &
sleep 7
run import -window root -resize 50% "png:$O/2-after.png"
wait
run glxinfo -B > "$O/glxinfo.txt" 2>&1
echo "gears after: $(grep -m1 -iE 'frames in' "$O/gears.txt")"
echo "renderer after: $(grep -m1 'renderer string' "$O/glxinfo.txt")"
dmesg | tail -n +$((D0+1)) | grep powervr | tee "$O/dmesg.txt"
grep -q 'device lost' "$O/dmesg.txt" && echo "DEVICE LOST"
md5sum "$O"/*.png
sh "$up" "$R" stop
