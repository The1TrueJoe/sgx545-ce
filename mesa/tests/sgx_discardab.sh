#!/bin/sh
# discard A/B on the CE5310 board: the DDK (i386 probe, as sgx_varyab.sh runs it) against ours (x86_64 build),
# a triangle whose right half is discarded (tools/isa/discard's scene), with
# the depth test and blending around it
cd /tmp/mesa
printf "#!/bin/sh\\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\\n" > ddk-probe.sh; chmod +x ddk-probe.sh
export PROBE_VS="attribute vec4 p; varying vec2 q; void main() { gl_Position = p; q = p.xy; }"
export PROBE_FS="precision mediump float; uniform vec4 c; varying vec2 q; void main() { if (q.x > 0.0) discard; gl_FragColor = c; }"
run() {
	name=$1; shift
	env "$@" PROBE_OUT=/tmp/mesa/dab-ddk-$name.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/dab-ddk-$name.log 2>&1
	env "$@" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/dab-ours-$name.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/dab-$name.log 2>&1
	a=$(md5sum < dab-ddk-$name.rgba); b=$(md5sum < dab-ours-$name.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r $(grep -o 'recoveries=[0-9]*' dab-$name.log)"
}
run plain
run less PROBE_CONFIG="8 8 8 8 24 0" PROBE_CLEARDEPTH=1 PROBE_DEPTH="513 1" PROBE_Z=0.5
run lequal_nowrite PROBE_CONFIG="8 8 8 8 24 0" PROBE_CLEARDEPTH=1 PROBE_DEPTH="515 0" PROBE_Z=0.5
run blend PROBE_BLEND="770 771" PROBE_COLOR="0 1 0 0.5"
