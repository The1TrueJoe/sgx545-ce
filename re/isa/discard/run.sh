#!/bin/sh
# discard tests: run.sh NAME LIBDIR(stock|dir) VAR=value ...  -> /tmp/dc/NAME.rgba, prints rc and new SGX dmesg lines
R=/opt/sgx-i386
N=$1; LIB=$2; shift 2
LP=$R/usr/lib:$R/lib
[ "$LIB" != stock ] && LP=/tmp/dc/$LIB:$LP
n=$(dmesg | wc -l)
env "$@" PROBE_OUT=/tmp/dc/$N.rgba LD_PRELOAD=$R/usr/lib/sgx-busidshim.so \
	$R/lib/ld-linux.so.2 --library-path $LP $R/bin/probe-st > /tmp/dc/$N.log 2>&1 &
P=$!
( sleep 30; kill -9 $P 2>/dev/null && echo "$N: watchdog killed" ) &
W=$!
wait $P; RC=$?
kill $W 2>/dev/null
echo "$N: rc=$RC $(grep -E 'shader|failed|link' /tmp/dc/$N.log | head -2)"
dmesg | tail -n +$((n+1)) | grep -iE 'recover|lockup|hang|SGX' | grep -v pvr-compat | tail -3
