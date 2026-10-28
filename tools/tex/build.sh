#!/bin/sh
# Build blitest (static x86_64 musl, for the EA3): GPU blits on a transfer context of its own.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/libpvr/abi/flags.sh"
zig cc -target x86_64-linux-musl -O2 -Wall $DEFS $INC -I$R/libpvr -static -o "$R/tools/tex/blitest" \
	"$R/tools/tex/blitest.c" "$R/tools/tex/sgxtex.c" "$R/libpvr/pvr.c"
