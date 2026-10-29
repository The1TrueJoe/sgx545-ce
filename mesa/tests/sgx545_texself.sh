#!/bin/sh
# texture cases on ours only (EA1: no DDK probe): md5 per case, to compare builds
cd /tmp/mesa
export PROBE_TEX=1 PROBE_ATTRS=2 PROBE_TEXPAT=1
export PROBE_VS="attribute vec4 p; attribute vec2 a1; varying vec2 v; void main() { gl_Position = p; v = p.xy + vec2(0.5, 0.5); }"
export PROBE_FS="precision mediump float; uniform sampler2D t0; varying vec2 v; void main() { gl_FragColor = texture2D(t0, v); }"
run() {
	name=$1; shift
	env "$@" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/self-$name.rgba ./gpuwatch.sh ./probe-tex-x86_64 > /tmp/mesa/self-$name.log 2>&1
	echo "$name $(md5sum < self-$name.rgba | cut -c1-12) $(grep -o 'recoveries=[0-9]*' self-$name.log)"
}
run n16 PROBE_TEXSIZE="16 16"
run l16 PROBE_TEXSIZE="16 16" PROBE_TEXFILTER="9729 9729"
run n64 PROBE_TEXSIZE="64 64"
run r1024 PROBE_TEXSIZE="1024 512"
run t4x64 PROBE_TEXSIZE="4 64"
run npot PROBE_TEXSIZE="12 6" PROBE_TEXWRAP="33071 33071"
run l8 PROBE_TEXSIZE="16 16" PROBE_TEXFMT=l8
run la88 PROBE_TEXSIZE="16 16" PROBE_TEXFMT=la88
run rgb565 PROBE_TEXSIZE="16 16" PROBE_TEXFMT=rgb565
