#!/bin/sh
# Build the native x86_64 replayer (static, musl): tools/replay/replay
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/libpvr/abi/flags.sh"
zig cc -target x86_64-linux-musl -O2 -Wall -static $DEFS $INC -I$R/libpvr -o "$R/tools/replay/replay" \
	"$R/tools/replay/replay.c" "$R/libpvr/pvr.c"
