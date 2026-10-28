#!/bin/sh
# Build pvrtrace for both ABIs: tools/trace/pvrtrace-{i386,x86_64}.so
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/libpvr/abi/flags.sh"
for t in x86:i386 x86_64:x86_64; do
	zig cc -target ${t%%:*}-linux-gnu.2.17 -O2 -Wall -Wno-unused-function -shared -fPIC $DEFS $INC \
		-o "$R/tools/trace/pvrtrace-${t#*:}.so" "$R/tools/trace/pvrtrace.c" -ldl -lpthread || exit 1
done
