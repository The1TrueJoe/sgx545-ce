#!/bin/sh
# Build tools/iso/iso (static x86_64 musl), the isolation-test client.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/libpvr/abi/flags.sh"
python3 "$R/tools/gen/mkusse.py" > "$R/tools/gen/usse_progs.h"
zig cc -target x86_64-linux-musl -O2 -Wall -Wno-unused-function -static $DEFS $INC -I"$R/libpvr" -I"$R/tools/gen" \
	-o "$R/tools/iso/iso" "$R/tools/iso/iso.c" "$R/tools/gen/sgxgen.c" "$R/libpvr/pvr.c" -lm
