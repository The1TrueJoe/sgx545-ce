#!/bin/sh
# texture A/B on the EA3: the DDK (i386 probe-tex) against ours (x86_64 build), same scene
cd /tmp/mesa
export PROBE_TEX=1 PROBE_ATTRS=2 PROBE_TEXPAT=1
export PROBE_VS="attribute vec4 p; attribute vec2 a1; varying vec2 v; void main() { gl_Position = p; v = p.xy + vec2(0.5, 0.5); }"
export PROBE_FS="precision mediump float; uniform sampler2D t0; varying vec2 v; void main() { gl_FragColor = texture2D(t0, v); }"
run() {
	name=$1; shift
	env "$@" PROBE_OUT=/tmp/mesa/ab-ddk-$name.rgba ./gpurun.sh /opt/sgx-i386/run.sh probe-tex > /dev/null 2>&1
	env "$@" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/ab-ours-$name.rgba ./gpurun.sh ./probe-tex-x86_64 > /tmp/mesa/ab-$name.log 2>&1
	a=$(md5sum < ab-ddk-$name.rgba); b=$(md5sum < ab-ours-$name.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r $(grep -o 'centre rgba([0-9,]*)' ab-$name.log) $(grep -i -c 'recovery' ab-$name.log)"
}
run n16 PROBE_TEXSIZE="16 16"
run l16 PROBE_TEXSIZE="16 16" PROBE_TEXFILTER="9729 9729"
run n64 PROBE_TEXSIZE="64 64"
run npot PROBE_TEXSIZE="12 6" PROBE_TEXWRAP="33071 33071"
run l8 PROBE_TEXSIZE="16 16" PROBE_TEXFMT=l8
run la88 PROBE_TEXSIZE="16 16" PROBE_TEXFMT=la88
run rgb565 PROBE_TEXSIZE="16 16" PROBE_TEXFMT=rgb565
# dependent samples (a USSE smp with the texture state in SA): swizzled,
# projected with a vec3 (gl4es' fixed-function lookups), with a bias
run dep_swz PROBE_TEXSIZE="16 16" PROBE_FS="precision mediump float; uniform sampler2D t0; varying vec2 v; void main() { gl_FragColor = texture2D(t0, v.yx); }"
run dep_proj3 PROBE_TEXSIZE="16 16" PROBE_TEXFILTER="9729 9729" PROBE_FS="precision mediump float; uniform sampler2D t0; varying vec2 v; void main() { gl_FragColor = texture2DProj(t0, vec3(v, 1.5)); }"
run dep_bias PROBE_TEXSIZE="16 16" PROBE_FS="precision mediump float; uniform sampler2D t0; varying vec2 v; void main() { gl_FragColor = texture2D(t0, v * 2.0, 0.5); }"
