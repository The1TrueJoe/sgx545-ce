#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# From the host: RAM-boot a powervr_sgx test kernel (an m2-drm CI artifact
# powervr-sgx-ea1-v1) on the EA1 and load its modules. Hold
# ~/Documents/GitHub/sgx545-work/locks/ea1.lock first.
#
#   ea1-ramboot.sh KERNDIR [ddk|open]
#
# ddk (default) loads the DDK microkernel, open loads sgx545-openhc-ukernel.bin;
# powervr_sgx prefers the open image when it is present, so it is always named.
# The kernel goes to /boot/once with drm/ci/ea-ramboot/S00aramboot, which
# deletes itself on the next boot; restore with `rm -rf /boot/once` and a plain
# reboot. The modules go to /root/m3/k. BOX names the box (the EA3 works too);
# S00ARAMBOOT another boot hook than this tree's (m2-drm a7d3df1's carries the
# bootstrap's ohc.* options over, which the EA3 needs for its network).
set -e
K=$1 FW=${2:-ddk}
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../../.." && pwd)
BOX=${BOX:?the address of the EA1 (DHCP moves it: 10.0.0.110, then 10.0.0.115)}
E() { sshpass -p openhc ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 root@$BOX "$@"; }
retry() { for i in 1 2 3 4 5; do "$@" && return 0; sleep 3; done; return 1; }
case $FW in
ddk) img=powervr_sgx/sgx545-ukernel-1.7.862890.bin ;;
open) img=powervr_sgx/sgx545-openhc-ukernel.bin ;;
*) echo "ddk or open" >&2; exit 2 ;;
esac
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
