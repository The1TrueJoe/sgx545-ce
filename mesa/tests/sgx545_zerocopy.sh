#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# Zero-copy vertex fetch against the gather (SGX545_DEBUG=zerocopy), on the
# box in /tmp/mesa (lib/, probe-x86_64, gpuwatch.sh): client arrays whose
# DMA sources are 16-byte aligned (separate arrays) and only 4-byte aligned
# (interleaved strides 12, 20, 28, 32).  IDENTICAL for every case, with
# zero-copy draws in the log, settles that DOUTD takes 4-byte-aligned sources.
cd /tmp/mesa
FS="precision mediump float; varying vec4 v; void main() { gl_FragColor = v; }"
run() {
	name=$1; shift
	for mode in gather zerocopy; do
		dbg=draw; [ $mode = zerocopy ] && dbg=draw,zerocopy
		env "$@" PROBE_FS="$FS" SGX545_DEBUG=$dbg LD_LIBRARY_PATH=/tmp/mesa/lib \
			PROBE_OUT=/tmp/mesa/zc-$mode-$name.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/zc-$mode-$name.log 2>&1
	done
	a=$(md5sum < zc-gather-$name.rgba); b=$(md5sum < zc-zerocopy-$name.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r zero-copy draws $(grep -c 'zero-copy draw' zc-zerocopy-$name.log)" \
		"$(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*' zc-zerocopy-$name.log | tr '\n' ' ')"
}
run sep4 PROBE_ATTRS="4" \
	PROBE_VS="attribute vec4 p; attribute vec4 a1; varying vec4 v; void main() { gl_Position = p; v = fract(a1 * 0.37); }"
run il1 PROBE_INTERLEAVE=1 PROBE_ATTRS="1" \
	PROBE_VS="attribute vec4 p; attribute float a1; varying vec4 v; void main() { gl_Position = p; v = fract(vec4(a1) * vec4(0.37, 0.11, 0.05, 1.0)); }"
run il3 PROBE_INTERLEAVE=1 PROBE_ATTRS="3" \
	PROBE_VS="attribute vec4 p; attribute vec3 a1; varying vec4 v; void main() { gl_Position = p; v = vec4(fract(a1 * 0.37), 1.0); }"
run il4z PROBE_INTERLEAVE=1 PROBE_Z=0.5 PROBE_ATTRS="4" \
	PROBE_VS="attribute vec4 p; attribute vec4 a1; varying vec4 v; void main() { gl_Position = p; v = fract(a1 * 0.37); }"
run il321 PROBE_INTERLEAVE=1 PROBE_ATTRS="3 2 1" \
	PROBE_VS="attribute vec4 p; attribute vec3 a1; attribute vec2 a2; attribute float a3; varying vec4 v; void main() { gl_Position = p; v = fract(vec4(a1, a3) * 0.37 + a2.xyxy * 0.11); }"
