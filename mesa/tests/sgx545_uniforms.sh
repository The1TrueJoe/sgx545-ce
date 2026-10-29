#!/bin/sh
# many vertex-shader uniforms against the DDK on the EA1 (gl4es' lighting
# shader reads 160 floats): the colour is the mean of u[0..n-1]
cd /tmp/mesa
printf '#!/bin/sh\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\n' > ddk-probe.sh; chmod +x ddk-probe.sh
for n in 4 16 24 32 40 48; do
	VS="attribute vec4 p; uniform vec4 u[$n]; varying vec4 v; void main() { gl_Position = p; vec4 s = vec4(0.0); for (int i = 0; i < $n; i++) s += u[i]; v = s / float($n) + vec4(0.0, 0.0, 0.0, 0.0) * p; }"
	FS="precision mediump float; varying vec4 v; void main() { gl_FragColor = v; }"
	PROBE_UARRAY=$n PROBE_VS="$VS" PROBE_FS="$FS" PROBE_OUT=/tmp/mesa/u-ddk.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/u-ddk.log 2>&1
	PROBE_UARRAY=$n PROBE_VS="$VS" PROBE_FS="$FS" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/u-ours.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/u-ours.log 2>&1
	a=$(md5sum < u-ddk.rgba); b=$(md5sum < u-ours.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "u[$n]: $r ours $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*\|sgx545:.*' u-ours.log | tr '\n' ' ') ddk $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*' u-ddk.log | tr '\n' ' ')"
done
