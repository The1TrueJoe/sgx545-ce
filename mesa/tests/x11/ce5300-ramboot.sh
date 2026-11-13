#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# From the host: RAM-boot a powervr_sgx test kernel (a CI artifact for a
# CE5300 board: bzImage and modules/) on the box BOX and load its modules.
# Take the box's lock first.
#
#   BOX=host BOX_PASS=... ce5300-ramboot.sh KERNDIR
#
# It loads the open microkernel, powervr_sgx/sgx545_ukernel.bin. The kernel goes to /boot/once with drm/ci/ea-ramboot/S00aramboot, which
# deletes itself on the next boot; restore with `rm -rf /boot/once` and a plain
# reboot. The modules go to /root/m3/k. S00ARAMBOOT names another boot hook
# than this tree's (one that carries the boot loader's kernel options over).
set -e
K=$1
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../../.." && pwd)
BOX=${BOX:?the address of the box}
E() { sshpass -p "${BOX_PASS:?the root password of the box}" ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 root@$BOX "$@"; }
retry() { for i in 1 2 3 4 5; do "$@" && return 0; sleep 3; done; return 1; }
img=powervr_sgx/sgx545_ukernel.bin
retry E 'mkdir -p /boot/once /root/m3/k/modules'
retry E 'cat > /boot/once/bzImage' < "$K/bzImage"
for m in "$K"/modules/*.ko; do retry E "cat > /root/m3/k/modules/$(basename "$m")" < "$m"; done
retry E 'cat > /etc/init.d/S00aramboot && chmod 755 /etc/init.d/S00aramboot' < "${S00ARAMBOOT:-$REPO/drm/ci/ea-ramboot/S00aramboot}"
retry E 'sync; reboot' || true
sleep 40
v=
for i in $(seq 1 40); do
	v=$(E 'uname -r' 2>/dev/null) && [ "$v" = 7.1.8-m2drm ] && break
	sleep 10
done
echo "kernel: $v"
retry E "/etc/init.d/S95ohc-display stop >/dev/null 2>&1; \
	for m in drm_exec drm_gpuvm gpu-sched drm_shmem_helper powervr_sgx; do \
		f=/root/m3/k/modules/\$m.ko; [ -f \$f ] || continue; \
		grep -q \"^\$(echo \$m | tr - _) \" /proc/modules && continue; \
		if [ \$m = powervr_sgx ]; then insmod \$f firmware=$img; else insmod \$f; fi; \
	done; touch /tmp/ohc-test-ramboot; ls /dev/dri; dmesg | grep -E 'powervr_sgx.*(microkernel|open)' | tail -3"
