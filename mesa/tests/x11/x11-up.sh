#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On the EA (powervr_sgx loaded, simpledrm on card0): Xorg with modesetting +
# glamor in the Debian root ROOT (mkroot.py), on our Mesa in ROOT/opt/sgx545.
#
#   x11-up.sh ROOT start|stop|run CMD...
#
# run: CMD inside the root against display :0, on our libGL/libEGL (glvnd
# vendor "mesa").
set -eu
R=$1; shift
env_mesa="LD_LIBRARY_PATH=/opt/sgx545/lib GBM_BACKENDS_PATH=/opt/sgx545/lib/gbm \
__EGL_VENDOR_LIBRARY_DIRS=/opt/sgx545/share/glvnd/egl_vendor.d __GLX_VENDOR_LIBRARY_NAME=mesa \
LIBGL_DRIVERS_PATH=/opt/sgx545/lib/dri"
# The root's dev, proc, sys and tmp are the box's own, bound in: while they are
# mounted, removing the root removes the box's /dev and /tmp. stop (and a start
# that fails) unmounts them.
mounts() {
	for d in dev proc sys tmp; do
		mountpoint -q "$R/$d" || { mkdir -p "$R/$d"; mount --bind "/$d" "$R/$d"; }
	done
}
unmounts() {
	for d in tmp sys proc dev; do
		mountpoint -q "$R/$d" && umount "$R/$d"
	done
	! grep -q " $R/" /proc/mounts
}
case $1 in
start)
	mounts
	mkdir -p "$R/etc/X11" "$R/var/log"
	cp "$(dirname "$0")/xorg.conf" "$R/etc/X11/xorg.conf"
	chroot "$R" /usr/bin/env $env_mesa /usr/lib/xorg/Xorg :0 -nolisten tcp -noreset \
		-config /etc/X11/xorg.conf -logfile /var/log/Xorg.0.log > "$R/var/log/xorg.out" 2>&1 &
	for i in $(seq 1 30); do [ -S /tmp/.X11-unix/X0 ] && break; sleep 1; done
	[ -S /tmp/.X11-unix/X0 ] && echo "Xorg :0 up" || {
		echo "Xorg did not start"
		tail -30 "$R/var/log/Xorg.0.log" "$R/var/log/xorg.out" 2>/dev/null
		unmounts
		exit 1
	}
	;;
stop)
	for p in $(ps | grep '/usr/lib/xorg/Xorg' | grep -v grep | awk '{print $1}'); do kill $p; done
	sleep 2
	unmounts
	;;
run)
	shift
	chroot "$R" /usr/bin/env $env_mesa DISPLAY=:0 "$@"
	;;
esac
