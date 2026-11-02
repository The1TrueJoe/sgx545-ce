#!/bin/sh
# the driver's own constant formats (list C kinds 0x10 and 0x11, opts.driver_consts) A/B on the EA1:
# lowp uniforms pre-converted to F16 from their C10 value, and literals in SA, against the DDK
cd /tmp/mesa
printf "#!/bin/sh\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\n" > ddk-probe.sh; chmod +x ddk-probe.sh
run() {
	name=$1 VS=$2 FS=$3
	PROBE_VS="$VS" PROBE_FS="$FS" PROBE_OUT=/tmp/mesa/dc-ddk-$name.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/dc-ddk-$name.log 2>&1
	PROBE_VS="$VS" PROBE_FS="$FS" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/dc-ours-$name.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/dc-ours-$name.log 2>&1
	a=$(md5sum < dc-ddk-$name.rgba); b=$(md5sum < dc-ours-$name.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r="DIFFER $(cmp -l dc-ddk-$name.rgba dc-ours-$name.rgba | wc -l) bytes"
	echo "$name: $r ours $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*\|sgx545:.*' dc-ours-$name.log | tr '\n' ' ') ddk $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*' dc-ddk-$name.log | tr '\n' ' ')"
}
VF="attribute vec4 p; varying float f; void main() { gl_Position = p; f = p.x * 0.5 + 0.5; }"
# gl4es' fog: a lowp colour mixed in mediump
export PROBE_COLOR="0.3 0.565 0.98 1"
run fog "$VF" "precision mediump float; uniform lowp vec4 c; varying float f; void main() { gl_FragColor = vec4(mix(vec3(0.1, 0.2, 0.3), c.rgb, f), 1.0); }"
# C10 steps that F16 shows (c = 144, 250 round differently by truncation)
export PROBE_COLOR="0.5647 0.9804 0.3 1"
run c10scale "$VF" "precision mediump float; uniform lowp vec4 c; varying float f; void main() { gl_FragColor = vec4(fract(c.xyz * (1.0 + f * 7.0)), 1.0); }"
# gl4es' 255.0 literal, in mediump (c.rgb * 255.0 itself is lowp: the DDK saturates it at 2)
run lit255 "$VF" "precision mediump float; uniform lowp vec4 c; varying float f; void main() { vec3 m = c.rgb; m = m * f; gl_FragColor = vec4(floor(m * 255.0) / 255.0, 1.0); }"
