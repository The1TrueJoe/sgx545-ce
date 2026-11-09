#!/bin/sh
# Offline oracle traces (tools/ukernel run.py, ukernel/run_ours.py): frame-fake built for this
# host (Linux or macOS; no GPU), one trace per case, into OUT (default gen/).
#   fake       the reference triangle, one frame
#   v2f-single two renders of one surface, each as its first (plain -R)
#   v2f, v3f   two and three frames, alternating RT entries (SGXGEN_ALTERNATE)
#   v2s        two renders, a new surface for the second
#   v2x        two renders, a new context and surface each (separate runs)
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
G=$R/tools/gen
OUT=${1:-$R/gen}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
. "$R/libpvr/abi/flags.sh"
X=
if [ "$(uname -s)" != Linux ]; then
	# Linux's ioctl encoding, so the trace's requests match the box's
	mkdir -p "$T/linux"
	cat > "$T/linux/ioctl.h" <<'H'
#pragma once
#include <sys/ioctl.h>
#undef _IOC
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR
#define _IOC(d, t, n, s) (((unsigned)(d) << 30) | ((unsigned)(s) << 16) | ((unsigned)(t) << 8) | (unsigned)(n))
#define _IO(t, n) _IOC(0U, t, n, 0)
#define _IOR(t, n, ty) _IOC(2U, t, n, sizeof(ty))
#define _IOW(t, n, ty) _IOC(1U, t, n, sizeof(ty))
#define _IOWR(t, n, ty) _IOC(3U, t, n, sizeof(ty))
H
	X="-D__linux__ -I$T"
fi
CC="${CC:-cc} -O2 -Wno-unused-function $X $DEFS $INC -I$R/libpvr"
python3 "$G/mkusse.py" > "$G/usse_progs.h"
$CC -c -Dioctl=fake_ioctl -Dmmap=fake_mmap -o "$T/pvr-fake.o" "$R/libpvr/pvr.c"
$CC -o "$T/frame-fake" "$G/frame.c" "$G/sgxgen.c" "$G/fakepvr.c" "$T/pvr-fake.o" -lm
mkdir -p "$OUT"
F="$T/frame-fake -n /dev/null"
$F -t "$OUT/fake.trace"
$F -R 2 -t "$OUT/v2f-single.trace"
SGXGEN_ALTERNATE=1 $F -R 2 -t "$OUT/v2f.trace"
SGXGEN_ALTERNATE=1 $F -R 3 -t "$OUT/v3f.trace"
$F -R 2 -S -t "$OUT/v2s.trace"
$F -R 2 -X -t "$OUT/v2x.trace"
