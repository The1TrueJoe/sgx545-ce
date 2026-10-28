#!/bin/sh
# Build the native x86_64 srvinit (static, musl): srvinit/srvinit
R=$(cd "$(dirname "$0")/.." && pwd)
. "$R/libpvr/abi/flags.sh"
zig cc -target x86_64-linux-musl -O2 -Wall -static $DEFS $INC -I$R/libpvr -o "$R/srvinit/srvinit" \
	"$R/srvinit/srvinit.c" "$R/libpvr/pvr.c"
