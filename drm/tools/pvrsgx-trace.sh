#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Trace powervr_sgx's jobs while a command runs, for drm/tools/pvrsgx-latency.py.
#
#   pvrsgx-trace.sh OUT COMMAND...
set -eu
OUT=$1
shift
T=/sys/kernel/tracing
[ -d $T/events ] || mount -t tracefs none $T
echo 0 > $T/tracing_on
echo > $T/trace
echo 8192 > $T/buffer_size_kb
for e in powervr_sgx gpu_scheduler/drm_sched_job_queue gpu_scheduler/drm_sched_job_run \
	gpu_scheduler/drm_sched_job_done; do
	echo 1 > $T/events/$e/enable
done
echo 'comm ~ "irq/*powervr*"' > $T/events/sched/sched_switch/filter 2>/dev/null || true
echo 1 > $T/events/sched/sched_wakeup/enable
echo 1 > $T/tracing_on
"$@"
echo 0 > $T/tracing_on
cat $T/trace > "$OUT"
for e in powervr_sgx gpu_scheduler sched/sched_wakeup; do echo 0 > $T/events/$e/enable; done
echo "trace: $(grep -c pvrsgx_kick "$OUT") kicks -> $OUT"
