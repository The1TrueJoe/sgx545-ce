#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# Minecraft 1.7.10, offline, on a 64-bit JVM inside the Debian root
# (x11-up.sh run), on the X server there: LWJGL 2's amd64 natives open a GLX
# window through libglvnd's libGL, which dispatches to our Mesa. No gl4es, no
# shims. The game files are mc-x64-stage.sh's copy of the box's
# openhc-minecraft install in $G (default /opt/mc-x64), never the install.
#
#   x11-up.sh ROOT run /opt/mc-x64/mc-x64.sh [USERNAME]
#
# As the install's minecraft-ab: OptiFine when staged (MC_OPTIFINE=0 leaves it
# out) and openHC's tweaker (MC_TWEAKS=0), through Mojang's launchwrapper.
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
TWEAK=
OF=$(ls "$G"/optifine/OptiFine_"$V"_*_MOD.jar 2>/dev/null | tail -n 1)
if [ -n "$OF" ] && [ "${MC_OPTIFINE:-1}" != 0 ]; then
	CPA="$OF:$CPA" TWEAK="--tweakClass optifine.OptiFineTweaker"
fi
if [ -f "$G/openhc-mc.jar" ] && [ "${MC_TWEAKS:-1}" != 0 ]; then
	CPA="$G/openhc-mc.jar:$G/$ASM:$CPA" TWEAK="$TWEAK --tweakClass openhc.mc.Tweaker"
fi
[ -n "$TWEAK" ] && CPA="$G/$LW:$CPA" MAIN=net.minecraft.launchwrapper.Launch
exec /opt/java/jre8-x64/bin/java -server -Xss1M -Xms256M -Xmx"${MC_XMX:-768M}" \
	-XX:+UseConcMarkSweepGC -Djava.awt.headless=true \
	-Djava.library.path="$G/natives" \
	-cp "$CPA" "$MAIN" \
	--username "${1:-Player}" --version "$V" --gameDir "$G" --assetsDir "$G/assets" \
	--assetIndex "$ASSET_INDEX" --uuid 00000000000000000000000000000000 --accessToken 0 \
	--userProperties {} --userType legacy \
	--width "${MC_WIDTH:-720}" --height "${MC_HEIGHT:-480}" $TWEAK
