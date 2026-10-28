#!/bin/sh
# Build pvrtrace for both ABIs: re/trace/pvrtrace-{i386,x86_64}.so
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/re/abi/flags.sh"
for t in x86:i386 x86_64:x86_64; do
	zig cc -target ${t%%:*}-linux-gnu.2.17 -O2 -Wall -Wno-unused-function -shared -fPIC $DEFS $INC \
		-o "$R/re/trace/pvrtrace-${t#*:}.so" "$R/re/trace/pvrtrace.c" -ldl -lpthread || exit 1
done
