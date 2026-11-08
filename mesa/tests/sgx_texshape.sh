#!/bin/sh
# texture shapes against the DDK on the EA1: power-of-two textures that aren't
# square (Minecraft's 64x32 player skin), twiddled, nearest
cd /tmp/mesa
printf '#!/bin/sh\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\n' > ddk-probe.sh; chmod +x ddk-probe.sh
export PROBE_TEX=1 PROBE_ATTRS=2 PROBE_TEXPAT=1
export PROBE_VS="attribute vec4 p; attribute vec2 a1; varying vec2 v; void main() { gl_Position = p; v = p.xy + vec2(0.5, 0.5); }"
export PROBE_FS="precision mediump float; uniform sampler2D t0; varying vec2 v; void main() { gl_FragColor = texture2D(t0, v); }"
for size in "16 16" "64 32" "32 64" "64 16" "16 64" "256 128" "8 32"; do
	name=$(echo $size | tr ' ' x)
	PROBE_TEXSIZE="$size" PROBE_OUT=/tmp/mesa/ts-ddk.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/ts-ddk.log 2>&1
	PROBE_TEXSIZE="$size" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/ts-ours.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/ts-ours.log 2>&1
	a=$(md5sum < ts-ddk.rgba); b=$(md5sum < ts-ours.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r $(grep -o 'recoveries=[0-9]*' ts-ours.log) $(grep -o 'recoveries=[0-9]*' ts-ddk.log)"
done
