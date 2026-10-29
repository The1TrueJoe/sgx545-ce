#!/bin/sh
# vertices of many attributes against the DDK on the EA1: 5 and 6 vec4
# attributes (20 and 24 dwords; Minecraft's lit entities have 5), the colour
# from the last ones
cd /tmp/mesa
printf '#!/bin/sh\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\n' > ddk-probe.sh; chmod +x ddk-probe.sh
run() {
	name=$1 ATTRS=$2 VS=$3
	FS="precision mediump float; varying vec4 v; void main() { gl_FragColor = v; }"
	PROBE_ATTRS="$ATTRS" PROBE_VS="$VS" PROBE_FS="$FS" PROBE_OUT=/tmp/mesa/at-ddk.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/at-ddk.log 2>&1
	PROBE_ATTRS="$ATTRS" PROBE_VS="$VS" PROBE_FS="$FS" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/at-ours.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/at-ours.log 2>&1
	a=$(md5sum < at-ddk.rgba); b=$(md5sum < at-ours.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r ours $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*\|sgx545:.*' at-ours.log | tr '\n' ' ') ddk $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*' at-ddk.log | tr '\n' ' ')"
}
run attr4 "4 4 4" "attribute vec4 p; attribute vec4 a1; attribute vec4 a2; attribute vec4 a3; varying vec4 v; void main() { gl_Position = p; v = fract(a3 * 0.37 + a2 * 0.11 + a1 * 0.05); }"
run attr5 "4 4 4 4" "attribute vec4 p; attribute vec4 a1; attribute vec4 a2; attribute vec4 a3; attribute vec4 a4; varying vec4 v; void main() { gl_Position = p; v = fract(a4 * 0.37 + a3 * 0.11 + a1 * 0.05); }"
run attr6 "4 4 4 4 4" "attribute vec4 p; attribute vec4 a1; attribute vec4 a2; attribute vec4 a3; attribute vec4 a4; attribute vec4 a5; varying vec4 v; void main() { gl_Position = p; v = fract(a5 * 0.37 + a4 * 0.11 + a2 * 0.05); }"
