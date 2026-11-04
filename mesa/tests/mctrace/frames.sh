#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On the box: the back buffer of a trace's frames, replayed on our desktop GL.
#   frames.sh TRACE OUTDIR FRAME...
# Each frame is a replay to just before its swap with eglretrace -D (a state
# dump with the framebuffer images): its --snapshot reads the front buffer,
# which a surfaceless pbuffer doesn't have. Writes OUTDIR/FRAME.json; the
# GL_BACK image comes out with mctrace/fbimage.py. Env: L (our Mesa,
# default /root/piglit/lib-pg21), A (amd64 apitrace, default /root/apitrace64).
T=${1:?trace} O=${2:?outdir}; shift 2
L=${L:-/root/piglit/lib-pg21} A=${A:-/root/apitrace64}
mkdir -p "$O"
LD_LIBRARY_PATH=$A/lib "$A/bin/apitrace" dump "$T" 2>/dev/null | grep -E "^[0-9]+ glXSwapBuffers" | cut -d' ' -f1 > "$O/swaps.txt"
for f in "$@"; do
	c=$(sed -n "${f}p" "$O/swaps.txt")
	[ -n "$c" ] || { echo "frames.sh: no frame $f"; continue; }
	/root/mc/gpuwatch.sh env LD_LIBRARY_PATH=$A/lib:$L:/root/piglit/lib __EGL_VENDOR_LIBRARY_FILENAMES=$L/50_mesa.json \
		LD_PRELOAD=$L/libEGL_mesa.so.0 WAFFLE_PLATFORM=surfaceless_egl \
		"$A/bin/eglretrace" -D $((c - 1)) "$T" > "$O/$f.json" 2> "$O/$f.err"
	echo "frame $f (call $c): $(tail -1 "$O/$f.err")"
done
