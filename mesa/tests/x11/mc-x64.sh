#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# Minecraft 1.7.10, vanilla, offline, on a 64-bit JVM inside the Debian root
# (x11-up.sh run), on the X server there: LWJGL 2's amd64 natives open a GLX
# window through libglvnd's libGL, which dispatches to our Mesa. No gl4es, no
# shims. The game files are a copy of the box's openhc-minecraft install in
# $G (default /opt/mc-x64), never the install itself.
#
#   x11-up.sh ROOT run /opt/mc-x64/mc-x64.sh [USERNAME]
G=${MC_DIR:-/opt/mc-x64}
V=1.7.10
. "$G/$V.launch" || exit 1
cd "$G" || exit 1
[ -f options.txt ] || cat > options.txt <<OPT
renderDistance:3
fancyGraphics:false
ao:0
particles:2
clouds:false
fboEnable:false
enableVsync:false
mipmapLevels:0
maxFps:60
guiScale:2
pauseOnLostFocus:false
snooperEnabled:false
OPT
CPA=
IFS=:
for j in $CP; do CPA="$CPA${CPA:+:}$G/$j"; done
unset IFS
exec /opt/java/jre8-x64/bin/java -server -Xss1M -Xms256M -Xmx"${MC_XMX:-768M}" \
	-XX:+UseConcMarkSweepGC -Djava.awt.headless=true \
	-Djava.library.path="$G/natives" \
	-cp "$CPA" "$MAIN" \
	--username "${1:-Player}" --version "$V" --gameDir "$G" --assetsDir "$G/assets" \
	--assetIndex "$ASSET_INDEX" --uuid 00000000000000000000000000000000 --accessToken 0 \
	--userProperties {} --userType legacy \
	--width "${MC_WIDTH:-720}" --height "${MC_HEIGHT:-480}"
