#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# The i386 libgl4esq.so (gl4es-arrayq.c) for tracing Minecraft above gl4es:
#   mesa/tests/mctrace/build.sh OUTDIR
# Its DT_NEEDED is libgl4es-real.so.1, an empty stub here (link only); on the
# box that name links to gl4es' libGL.so.1.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
O=${1:?outdir}
mkdir -p "$O"
echo > "$O/stub.c"
zig cc -target x86-linux-gnu.2.17 -shared -fPIC -o "$O/libgl4es-real.so.1" -Wl,-soname,libgl4es-real.so.1 "$O/stub.c"
zig cc -target x86-linux-gnu.2.17 -shared -fPIC -O1 -o "$O/libgl4esq.so" "$HERE/gl4es-arrayq.c" \
	-Wl,--no-as-needed "$O/libgl4es-real.so.1" -ldl
rm -f "$O/stub.c" "$O/libgl4es-real.so.1"
