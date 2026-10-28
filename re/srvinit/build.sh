#!/bin/sh
# Build the native x86_64 srvinit (static, musl): re/srvinit/srvinit
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/re/abi/flags.sh"
zig cc -target x86_64-linux-musl -O2 -Wall -static $DEFS $INC -I"$R/re/replay" -o "$R/re/srvinit/srvinit" \
	"$R/re/srvinit/srvinit.c" "$R/re/replay/pvr.c"
