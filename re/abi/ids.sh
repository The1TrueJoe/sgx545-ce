#!/bin/sh
# Bridge index of every PVRSRV_BRIDGE_* in this build: re/abi/bridge-ids.txt ("index NAME").
# Each id becomes the size of a common symbol, so the compiler does the #if logic and nm reads it.
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/re/abi/flags.sh"
NAMES=$(grep -h -o "define[[:space:]]*PVRSRV_BRIDGE_[A-Z0-9_]*[[:space:]]*PVRSRV_IOWR" "$R/src/services4/include/pvr_bridge.h" \
	"$R/src/services4/include/sgx_bridge.h" | awk '{print $2}' | sort -u)
{
	echo '#include "abi.c"'
	for n in $NAMES; do
		printf '#ifdef %s\nchar n_%s[_IOC_NR(%s)+1];\n#endif\n' "$n" "$n" "$n"
	done
} > "$R/re/abi/ids.c"
O=$(mktemp)
zig cc -target x86-linux-musl -c -fcommon $DEFS $INC -I"$R/re/abi" -o "$O" "$R/re/abi/ids.c" 2>&1 | grep -v "unused during"
nm -S "$O" | while read -r _ sz _ sym; do echo "$((0x$sz - 1)) ${sym#n_PVRSRV_BRIDGE_}"; done | sort -n > "$R/re/abi/bridge-ids.txt"
rm -f "$O"
