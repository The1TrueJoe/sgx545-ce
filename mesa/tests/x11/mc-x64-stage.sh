#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On the box: copy what Minecraft needs from the Minecraft install into
# the Debian root, read-only on the install's side. The game directory is
# ROOT/opt/mc-x64; saves, options and logs live there and nowhere else.
#
#   mc-x64-stage.sh ROOT
#
# Takes the install's game files (libraries, versions, assets, natives,
# OptiFine), and the A/B rig's options and benchmark
# world (/opt/mc-ab), so a run here compares with that rig's numbers.
set -eu
R=$1
I=/opt/minecraft/rootfs
M=$I/opt/minecraft
A=$I/opt/mc-ab
G=$R/opt/mc-x64
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$G"
for d in libraries versions assets natives optifine; do
	[ -e "$M/$d" ] || continue     # OptiFine only when installed
	[ -e "$G/$d" ] || cp -a "$M/$d" "$G/"
done
cp "$M/1.7.10.launch" "$G/"
for f in options.txt optionsof.txt; do
	[ -f "$G/$f" ] || { [ -f "$A/$f" ] && cp "$A/$f" "$G/"; } || true
done
[ -d "$G/bench-world" ] || { [ -d "$A/bench-world" ] && cp -a "$A/bench-world" "$G/"; } || true
cp "$here/mc-x64.sh" "$G/" && chmod +x "$G/mc-x64.sh"
du -sh "$G"
