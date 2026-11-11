#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# From the host: the desktop lag measurements on a CE5300 box, every variant
# through every workload (lag-box.sh, lag-wl.sh), then lag-analyze.py.
#
#   BOX=host BOX_PASS=... lag-run.sh MESA_USR OUT VARIANT...
#
# MESA_USR is a Mesa install's usr/ (lib/ and share/, with GLX) that goes to
# DESK/opt/sgx. The box must be up on powervr_sgx with the monitor attached and
# the desktop root unpacked at DESK (smoke.sh does that once); hold the box's
# lock. WORKLOADS overrides the list (default: all of them).
set -eu
MESA=$1 OUT=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
BOX=${BOX:?the address of the box}
DESK=${DESK:-/opt/desk}
WORKLOADS=${WORKLOADS:-idle drag term menus drag-gl firefox}
# ssh with retries: the box's sshd turns down connections that come fast
# (255: ssh's own failure, not the command's)
E() {
	for i in 1 2 3 4 5 6; do
		sshpass -p "${BOX_PASS:?the root password of the box}" ssh -o PubkeyAuthentication=no \
			-o PreferredAuthentications=password -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
			-o LogLevel=ERROR -o ConnectTimeout=8 -o ServerAliveInterval=15 root@$BOX "$@" &&
			r=0 || r=$?
		[ $r -ne 255 ] && return $r
		sleep 3
	done
	return $r
}
mkdir -p "$OUT"
E "test -e /dev/dri/renderD128 && test -d $DESK/usr/bin && ! pidof Xorg" || {
	echo "lag-run.sh: the box needs powervr_sgx up, the root at $DESK, and no X running" >&2
	exit 1
}
E "uname -a; cat /proc/cpuinfo | grep -m1 'model name'; cat /sys/class/drm/card0/device/driver/module/srcversion 2>/dev/null; true" > "$OUT/box.txt"
# the Mesa under test
# (the box's umask is 077: a tree only root can read leaves "desk" on the
# root's own Mesa, silently; busybox's chmod -R fails on the dangling dri
# links, and that's fine)
(cd "$MESA" && tar cf - lib share) | E "rm -rf $DESK/opt/sgx && mkdir -m 755 -p $DESK/opt/sgx && \
	tar xf - -C $DESK/opt/sgx && { chmod -R a+rX $DESK/opt/sgx 2>/dev/null; true; }"
E "rm -rf /tmp/lagrig && mkdir -p /tmp/lagrig"
for f in ../desktop-up.sh ../xorg.conf lag-wl.sh lag-box.sh; do
	E "cat > /tmp/lagrig/$(basename $f)" < "$here/$f"
done
for v in "$@"; do
	echo "== $v"
	E "rm -rf /tmp/lagout/$v; SCANOUT=${SCANOUT:-} SCANOUT_BYTES=${SCANOUT_BYTES:-} \
		sh /tmp/lagrig/lag-box.sh $DESK /tmp/lagrig /tmp/lagout/$v $v $WORKLOADS" || echo "$v failed"
	mkdir -p "$OUT/$v"
	E "cd /tmp/lagout/$v && tar cf - ." | tar xf - -C "$OUT/$v"
	E "rm -rf /tmp/lagout/$v"
done
python3 -I "$here/lag-analyze.py" "$OUT" | tee "$OUT/summary.md"
