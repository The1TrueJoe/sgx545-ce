#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On a CE5300 box with powervr_sgx loaded (simpledrm on card0, the GPU's render
# node): an Xfce desktop on the monitor from the Debian root ROOT
# (desktop-root.yml), Xorg modesetting with glamor on our Mesa in ROOT/opt/sgx.
#
#   desktop-up.sh ROOT start|stop|run CMD...
#
# The box's dev, proc, sys, tmp and run are bound into the root: run carries
# udev's database, so libinput finds the USB keyboard and mouse. Never remove
# the root while they are mounted (stop unmounts them).
set -eu
R=$1; shift
here=$(cd "$(dirname "$0")" && pwd)
env_mesa="LD_LIBRARY_PATH=/opt/sgx/lib GBM_BACKENDS_PATH=/opt/sgx/lib/gbm \
__EGL_VENDOR_LIBRARY_DIRS=/opt/sgx/share/glvnd/egl_vendor.d __GLX_VENDOR_LIBRARY_NAME=mesa \
LIBGL_DRIVERS_PATH=/opt/sgx/lib/dri"
# mesa-sgx installed as a package (docs/INSTALL.md) needs no environment
[ -e "$R/etc/ld.so.conf.d/00-sgx.conf" ] && env_mesa=
# nor does a root with no Mesa of ours at all (Debian's Mesa, e.g. after apt remove mesa-sgx)
[ -d "$R/opt/sgx/lib" ] || env_mesa=
# XENV: more environment for Xorg only (SGX_DEBUG=..., MESA_SHADER_CAPTURE_PATH=...)
XENV=${XENV:-}
binds="dev proc sys tmp run"
mounts() {
	for d in $binds; do
		mountpoint -q "$R/$d" || { mkdir -p "$R/$d"; mount --bind "/$d" "$R/$d"; }
	done
	mountpoint -q "$R/dev/pts" || mount --bind /dev/pts "$R/dev/pts"
}
unmounts() {
	mountpoint -q "$R/dev/pts" && umount "$R/dev/pts"
	for d in run tmp sys proc dev; do
		mountpoint -q "$R/$d" && umount "$R/$d"
	done
	! grep -q " $R/" /proc/mounts
}
case $1 in
start)
	mounts
	mkdir -p "$R/etc/X11" "$R/var/log"
	cp "$here/xorg.conf" "$R/etc/X11/xorg.conf"
	chroot "$R" /usr/bin/env $env_mesa $XENV /usr/lib/xorg/Xorg :0 vt1 -nolisten tcp -noreset \
		-config /etc/X11/xorg.conf -logfile /var/log/Xorg.0.log > "$R/var/log/xorg.out" 2>&1 &
	for i in $(seq 1 30); do [ -S /tmp/.X11-unix/X0 ] && break; sleep 1; done
	[ -S /tmp/.X11-unix/X0 ] || {
		echo "Xorg did not start"
		tail -30 "$R/var/log/Xorg.0.log" "$R/var/log/xorg.out" 2>/dev/null
		unmounts
		exit 1
	}
	# the session as the root's user "desk", its own D-Bus session bus
	chroot "$R" /usr/bin/env -i HOME=/home/desk USER=desk SHELL=/bin/bash DISPLAY=:0 \
		PATH=/usr/local/bin:/usr/bin:/bin XDG_RUNTIME_DIR=/tmp/desk-run $env_mesa \
		/usr/sbin/runuser -u desk -- sh -c 'mkdir -p -m 700 /tmp/desk-run; exec dbus-launch --exit-with-session startxfce4' \
		> "$R/var/log/xfce.out" 2>&1 &
	echo "Xorg :0 up, Xfce starting"
	;;
stop)
	for p in $(ps | grep -e 'xfce4-session' -e 'xfwm4' -e 'xfdesktop' -e 'firefox' -e 'epiphany' |
		grep -v grep | awk '{print $1}'); do kill $p 2>/dev/null; done
	sleep 2
	for p in $(ps | grep '/usr/lib/xorg/Xorg' | grep -v grep | awk '{print $1}'); do kill $p; done
	sleep 2
	unmounts
	;;
run)
	shift
	chroot "$R" /usr/bin/env $env_mesa DISPLAY=:0 HOME=/home/desk USER=desk XDG_RUNTIME_DIR=/tmp/desk-run \
		/usr/sbin/runuser -u desk -- "$@"
	;;
esac
