#!/bin/sh
# Dump every bridge/microkernel struct layout for i386 and x86_64 with this build's defines.
# Usage: libpvr/abi/layouts.sh   (from the repo root; writes libpvr/abi/layout-<arch>.txt)
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/libpvr/abi/flags.sh"
for t in x86 x86_64; do
	zig cc -target $t-linux-musl -fsyntax-only $DEFS $INC -Xclang -fdump-record-layouts-complete \
		"$R/libpvr/abi/abi.c" 2>&1 | grep -v "unused during compilation" | sed "s|$R/||g" > "$R/libpvr/abi/layout-$t.txt"
done
