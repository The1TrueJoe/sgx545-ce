#!/bin/sh
# Build the native x86_64 replayer (static, musl): re/replay/replay
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/re/abi/flags.sh"
zig cc -target x86_64-linux-musl -O2 -Wall -static $DEFS $INC -o "$R/re/replay/replay" \
	"$R/re/replay/replay.c" "$R/re/replay/pvr.c"
