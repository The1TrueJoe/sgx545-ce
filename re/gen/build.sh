#!/bin/sh
# Build the frame generator (static x86_64 musl): re/gen/frame, and re/gen/frame-fake,
# the same program on fakepvr.c instead of the kernel driver (runs anywhere, touches no GPU).
# usse_progs.h is assembled from source text by mkusse.py (re/isa's encoder).
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
G=$R/re/gen
. "$R/re/abi/flags.sh"
CC="zig cc -target x86_64-linux-musl -O2 -Wall -Wno-unused-function $DEFS $INC -I$R/re/replay"
python3 "$G/mkusse.py" > "$G/usse_progs.h"
$CC -static -o "$G/frame" "$G/frame.c" "$G/sgxgen.c" "$R/re/replay/pvr.c" -lm
$CC -c -Dioctl=fake_ioctl -Dmmap=fake_mmap -o "$G/pvr-fake.o" "$R/re/replay/pvr.c"
$CC -static -o "$G/frame-fake" "$G/frame.c" "$G/sgxgen.c" "$G/fakepvr.c" "$G/pvr-fake.o" -lm
rm -f "$G/pvr-fake.o"
