#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# From the host: where piglit tests crash, on a box that has piglit in
# /root/piglit (drive-deqp.py's layout). Each test runs under segvtrace.so
# (built from segvtrace.c) the way drive-deqp.py runs piglit, and every frame
# of a crash is symbolized here against the same Mesa build with its debug
# info.
#
#   BOX=host BOX_PASS=... crash-triage.sh MESA_LIB SEGVTRACE_SO OUT TEST...
#
# MESA_LIB is a Mesa build's usr/lib (mg-stage/x86_64-gl-HASH/usr/lib); a
# TEST is piglit's binary and its arguments, e.g. "depth-tex-modes -auto -fbo".
set -eu
LIB=$1 SO=$2 OUT=$3
shift 3
BOX=${BOX:?the address of the box}
SYM=${SYM:-/opt/homebrew/opt/llvm/bin/llvm-symbolizer}
P=/root/piglit
E() { sshpass -p "${BOX_PASS:?the root password of the box}" ssh -o PubkeyAuthentication=no -o PreferredAuthentications=password \
	-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 root@$BOX "$@"; }
mkdir -p "$OUT"
(cd "$LIB" && tar cf - .) | E "rm -rf $P/lib-triage && mkdir -p $P/lib-triage && tar xf - -C $P/lib-triage && \
	printf '{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"$P/lib-triage/libEGL_mesa.so.0\"}}\n' > $P/lib-triage/50_mesa.json"
E "cat > $P/segvtrace.so" < "$SO"
env="LD_LIBRARY_PATH=$P/lib-triage:$P/lib:$P/piglit/lib __EGL_VENDOR_LIBRARY_FILENAMES=$P/lib-triage/50_mesa.json \
LD_PRELOAD=$P/lib-triage/libEGL_mesa.so.0:$P/segvtrace.so PIGLIT_PLATFORM=surfaceless_egl PIGLIT_NO_WINDOW=1 \
PIGLIT_SOURCE_DIR=$P/piglit SGX_DEBUG=hangabort"
for t in "$@"; do
	name=$(echo "$t" | cut -d' ' -f1)
	E "cd $P/piglit && flock /tmp/gpu.lock env $env ./bin/$t" > "$OUT/$name.txt" 2>&1 || true
	# object+offset frames of ours, symbolized with the build's debug info
	grep '^segvtrace: #' "$OUT/$name.txt" | while read -r _ n obj _; do
		f=${obj%%+*} off=${obj##*+}
		case $f in
		$P/lib-triage/*) l="$LIB/${f#$P/lib-triage/}"
			echo "$n $(basename "$f") $("$SYM" --obj="$l" "$off" 2>/dev/null | head -2 | tr '\n' ' ')" ;;
		*) echo "$n $obj" ;;
		esac
	done > "$OUT/$name.bt"
	echo "== $t"
	grep -m1 '^segvtrace: signal' "$OUT/$name.txt" || echo "   (no crash)"
	head -12 "$OUT/$name.bt"
done
E "rm -rf $P/lib-triage $P/segvtrace.so"
