#!/bin/sh
# On the RAM-booted EA: load powervr_sgx and run the uAPI tests.
#
#   run.sh <dir with modules/ and powervr_sgx_test> [test globs...]
#
# Holds /tmp/gpu.lock throughout. The microkernel is cut from the box's own
# libsrv_init (srvinit/fetch-firmware.sh does the same on a host) into
# /lib/firmware/powervr_sgx/; it is never copied off the box.
set -eu
D=$1
shift
exec 9>/tmp/gpu.lock
flock -n 9 || { echo "gpu.lock held by someone else" >&2; exit 1; }

FW=/lib/firmware/powervr_sgx/sgx545-ukernel-1.7.862890.bin
FW_SHA=73dce0885c45ecbe691f86305b3645e525e7a5e18de3de3b5a469d0df8850349
if [ "$(sha256sum "$FW" 2>/dev/null | cut -d' ' -f1)" != "$FW_SHA" ]; then
	mkdir -p "${FW%/*}"
	dd if=/opt/sgx-i386/usr/lib/libsrv_init.so.1.7.862890 of="$FW" bs=1 \
		skip=$((0x2000)) count=$((0xcf68)) 2>/dev/null
	[ "$(sha256sum "$FW" | cut -d' ' -f1)" = "$FW_SHA" ] || { echo "firmware sha mismatch" >&2; exit 1; }
fi

uname -r
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
if ! grep -q '^powervr_sgx ' /proc/modules; then
	for m in drm_exec drm_gpuvm gpu-sched drm_shmem_helper powervr_sgx; do
		[ -f "$D/modules/$m.ko" ] && insmod "$D/modules/$m.ko"
	done
fi
dmesg | grep -i -E 'powervr|pvrsgx|sgx' | tail -30
cat /sys/kernel/debug/dri/*/status 2>/dev/null || true
rc=0
"$D/powervr_sgx_test" "$@" || rc=$?
cat /sys/kernel/debug/dri/*/status 2>/dev/null || true
dmesg | grep -i -E 'powervr|pvrsgx' | tail -10
exit $rc
