#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# Where the time goes on the new stack (docs/PERF.md): perf record of glmark2's
# default scenes in a 720x480 window and of the GPU microbenchmark, each with
# SGX545_DEBUG=stats, on the EA under powervr_sgx (run-suite.sh's setup).
# Minecraft's profile comes from mc-oldnew.sh with MC_PROFILE=SECS.
# Reports (perf's top DSOs and symbols, the driver's stats) in /root/perf/profile/.
#
#   profile-new.sh
set -u
O=/root/perf/profile M=/root/m3 R=/opt/x64root B=/root/perf/bin
mkdir -p "$O" "$R/var/tmp/perf"
exec 9>/tmp/gpu.lock
flock -n 9 || { echo "profile-new: /tmp/gpu.lock is held" >&2; exit 1; }
report() { chroot $R /usr/bin/perf report -i "$1" --stdio --no-children --sort dso,symbol --percent-limit 0.5 2>/dev/null | grep -v '^$' | head -80; }

$M/x11-up.sh $R start > /dev/null
$M/x11-up.sh $R run env -u MESA_GL_VERSION_OVERRIDE -u MESA_GLSL_VERSION_OVERRIDE SGX545_DEBUG=stats \
	/usr/bin/perf record -q -F 499 -g -o /var/tmp/perf/glmark2.data glmark2 -s 720x480 > "$O/glmark2.log" 2>&1
report $R/var/tmp/perf/glmark2.data > "$O/glmark2.perf.txt"

# (with the root's dev bound in, which x11-up.sh's start does)
cp $B/sgx545_perfmicro-x86_64 $R/var/tmp/perf/
mkdir -p $R/var/tmp/perf/mesa && cp -a ${MESA_LIB:-/root/deqp/lib-m3c}/. $R/var/tmp/perf/mesa/
chroot $R /usr/bin/env LD_LIBRARY_PATH=/var/tmp/perf/mesa EGL_PLATFORM=surfaceless SGX545_DEBUG=stats \
	/usr/bin/perf record -q -F 499 -g -o /var/tmp/perf/micro.data /var/tmp/perf/sgx545_perfmicro-x86_64 > "$O/micro.log" 2>&1
report $R/var/tmp/perf/micro.data > "$O/micro.perf.txt"
$M/x11-up.sh $R stop
rm -rf $R/var/tmp/perf
grep -h 'glmark2 Score\|^fill\|^tex\|^verts' "$O/glmark2.log" "$O/micro.log"
