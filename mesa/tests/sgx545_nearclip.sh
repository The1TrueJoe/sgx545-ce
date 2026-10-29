#!/bin/sh
# triangles crossing the eye plane (a vertex behind the camera, as Minecraft's
# first-person arm has), textured, against the DDK on the EA1
cd /tmp/mesa
printf '#!/bin/sh\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\n' > ddk-probe.sh; chmod +x ddk-probe.sh
export PROBE_TEX=1 PROBE_ATTRS=2 PROBE_TEXPAT=1 PROBE_TEXSIZE="16 16"
# eye z from -3 (in front) to +1 (behind) across the triangle; a simple perspective
P="vec4 e = vec4(p.x * 2.0, p.y * 2.0, -1.0 + p.y * 4.0, 1.0); gl_Position = vec4(e.x * 1.5, e.y * 1.5, -e.z * 1.02 - 0.2, -e.z);"
run() {
	name=$1 VS=$2 FS=$3
	PROBE_VS="$VS" PROBE_FS="$FS" PROBE_OUT=/tmp/mesa/nc-ddk-$name.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/nc-ddk.log 2>&1
	PROBE_VS="$VS" PROBE_FS="$FS" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/nc-ours-$name.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/nc-ours.log 2>&1
	a=$(md5sum < nc-ddk-$name.rgba); b=$(md5sum < nc-ours-$name.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r ours $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*\|sgx545:.*' nc-ours.log | tr '\n' ' ') ddk $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*' nc-ddk.log | tr '\n' ' ')"
}
run plain "attribute vec4 p; attribute vec2 a1; varying vec2 v; void main() { v = p.xy + vec2(0.5, 0.5); $P }" \
	"precision mediump float; uniform sampler2D t0; varying vec2 v; void main() { gl_FragColor = texture2D(t0, v); }"
run proj4discard "attribute vec4 p; attribute vec2 a1; varying vec4 v; void main() { v = vec4(p.xy + vec2(0.5, 0.5), 0.0, 1.0); $P }" \
	"precision mediump float; uniform sampler2D t0; varying vec4 v; void main() { vec4 c = texture2DProj(t0, v); if (c.a * 255.0 <= 0.0) discard; gl_FragColor = c; }"
run front "attribute vec4 p; attribute vec2 a1; varying vec2 v; void main() { v = p.xy + vec2(0.5, 0.5); vec4 e = vec4(p.x * 2.0, p.y * 2.0, -2.0 + p.y * 1.0, 1.0); gl_Position = vec4(e.x * 1.5, e.y * 1.5, -e.z * 1.02 - 0.2, -e.z); }" \
	"precision mediump float; uniform sampler2D t0; varying vec2 v; void main() { gl_FragColor = texture2D(t0, v); }"
