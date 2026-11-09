#!/bin/sh
# Run an i386 DDK program under pvrtrace on the box, with a watchdog.
# Usage: trace.sh <out.bin> <program> [args...]   (program: path, or a name in /opt/sgx-i386/bin)
R=/opt/sgx-i386
OUT=$1; shift
BIN=$1; shift
case "$BIN" in /*) ;; *) BIN=$R/bin/$BIN ;; esac
PVRTRACE_OUT=$OUT LD_PRELOAD="$R/usr/lib/sgx-busidshim.so $R/usr/lib/pvrtrace.so" \
	"$R/lib/ld-linux.so.2" --library-path "$R/usr/lib:$R/lib" "$BIN" "$@" &
P=$!
( sleep ${PVRTRACE_WATCHDOG:-60}; kill -9 $P 2>/dev/null && echo "trace.sh: watchdog killed $BIN" ) &
W=$!
wait $P; RC=$?
kill $W 2>/dev/null
echo "trace.sh: rc=$RC"
