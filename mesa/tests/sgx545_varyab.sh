#!/bin/sh
# varyings A/B on the EA1: the DDK (i386 probe, tools/probe built with zig cc -target x86-linux-gnu.2.17) against ours (x86_64), varyings st packs into shared slots
cd /tmp/mesa
export PROBE_ATTRS=1
printf "#!/bin/sh\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\n" > ddk-probe.sh; chmod +x ddk-probe.sh
run() {
	name=$1 VS=$2 FS=$3
	PROBE_VS="$VS" PROBE_FS="$FS" PROBE_OUT=/tmp/mesa/va-ddk-$name.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/va-ddk-$name.log 2>&1
	PROBE_VS="$VS" PROBE_FS="$FS" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/va-ours-$name.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/va-ours-$name.log 2>&1
	a=$(md5sum < va-ddk-$name.rgba); b=$(md5sum < va-ours-$name.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r ours $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*\|sgx545:.*' va-ours-$name.log | tr '\n' ' ') ddk $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*' va-ddk-$name.log | tr '\n' ' ')"
}
V3="attribute vec4 p; varying vec3 a; varying vec3 b; varying float f; void main() { gl_Position = p; a = vec3(p.x * 0.5 + 0.5, 0.25, p.y * 0.5 + 0.5); b = vec3(0.75, p.y * 0.5 + 0.5, 0.5); f = p.x * 0.25 + 0.5; }"
run v33f "$V3" "precision mediump float; varying vec3 a; varying vec3 b; varying float f; void main() { gl_FragColor = vec4(a.x, b.y, a.z * f, b.z); }"
run v33f_all "$V3" "precision mediump float; varying vec3 a; varying vec3 b; varying float f; void main() { gl_FragColor = vec4(a.y + b.x - 0.5, b.y * f, a.z, 1.0); }"
run v2222 "attribute vec4 p; varying vec2 a; varying vec2 b; varying vec2 c; varying vec2 d; void main() { gl_Position = p; a = p.xy * 0.5 + 0.5; b = vec2(0.25, 0.75); c = p.yx * 0.5 + 0.5; d = vec2(0.5, 1.0); }" "precision mediump float; varying vec2 a; varying vec2 b; varying vec2 c; varying vec2 d; void main() { gl_FragColor = vec4(a.x, b.y, c.x * d.x, d.y); }"
