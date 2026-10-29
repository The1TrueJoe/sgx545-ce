#!/bin/sh
# texture reads the PDS does before the shader runs (non-dependent), against
# the DDK on the EA1: a projected read of a whole vec4 varying and a plain
# read of a vec4 varying's .xy (gl4es' fixed-function texturing)
cd /tmp/mesa
printf '#!/bin/sh\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\n' > ddk-probe.sh; chmod +x ddk-probe.sh
export PROBE_TEX=1 PROBE_ATTRS=2 PROBE_TEXPAT=1 PROBE_TEXSIZE="16 16"
V4="attribute vec4 p; attribute vec2 a1; varying vec4 v; void main() { gl_Position = p; v = vec4(p.xy * 0.5 + vec2(0.5, 0.5), 0.0, 1.0) * (1.5 + p.x * 0.5); }"
run() {
	name=$1 VS=$2 FS=$3
	PROBE_VS="$VS" PROBE_FS="$FS" PROBE_OUT=/tmp/mesa/tp-ddk-$name.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/tp-ddk-$name.log 2>&1
	PROBE_VS="$VS" PROBE_FS="$FS" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/tp-ours-$name.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/tp-ours-$name.log 2>&1
	a=$(md5sum < tp-ddk-$name.rgba); b=$(md5sum < tp-ours-$name.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r ours $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*\|sgx545:.*' tp-ours-$name.log | tr '\n' ' ') ddk $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*' tp-ddk-$name.log | tr '\n' ' ')"
}
run proj4 "$V4" "precision mediump float; uniform sampler2D t0; varying vec4 v; void main() { gl_FragColor = texture2DProj(t0, v); }"
run xy4 "$V4" "precision mediump float; uniform sampler2D t0; varying vec4 v; void main() { gl_FragColor = texture2D(t0, v.xy); }"
run proj4c "$V4" "precision mediump float; uniform sampler2D t0; varying vec4 v; void main() { gl_FragColor = texture2DProj(t0, v) * vec4(0.5, 1.0, 1.0, 1.0); }"
