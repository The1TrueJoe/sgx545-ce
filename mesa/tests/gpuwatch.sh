#!/bin/sh
# on the EA3: run a GPU client under the GPU lock, and kill it (and every
# glretrace/java) at the first new SGX recovery, so one bad frame costs one
# recovery and not a storm that wedges the GPU. GPUWATCH_KILL=0 only counts
# them (a reference run that has to finish).
#   gpuwatch.sh CMD...
t0=$(cut -d' ' -f1 /proc/uptime)
# GPU_LOCK_HELD=1: the caller holds the lock for a whole series of runs
if [ -n "$GPU_LOCK_HELD" ]; then "$@" & else flock /tmp/gpu.lock "$@" & fi
pid=$!
while kill -0 $pid 2>/dev/null; do
	n=$(dmesg | grep 'Recovery triggered' | awk -F'[][]' -v t="$t0" '$2 + 0 > t { n++ } END { print n + 0 }')
	if [ "$n" != 0 ] && [ "${GPUWATCH_KILL:-1}" != 0 ]; then
		echo "gpuwatch: SGX recovery after $(awk -v a="$t0" -v b="$(cut -d' ' -f1 /proc/uptime)" 'BEGIN{printf "%.1f", b-a}') s: killing the client"
		kill -9 $pid 2>/dev/null
		killall -9 glretrace java probe-x86_64 2>/dev/null
		break
	fi
	sleep 0.2
done
wait $pid 2>/dev/null
echo "gpuwatch: rc=$? recoveries=$(dmesg | grep 'Recovery triggered' | awk -F'[][]' -v t="$t0" '$2 + 0 > t { n++ } END { print n + 0 }')"
