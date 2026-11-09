#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On a CE5300 box with powervr_sgx RAM-booted: docs/INSTALL.md's userspace steps in the trixie
# desktop root at $R, as a user would run them (apt), then the rollback. Log on stdout.
set -u
R=/opt/desk; UP=$R/root/rig/desktop-up.sh; O=/tmp/e2e
F=/lib/firmware/powervr_sgx
rm -rf $O; mkdir -p -m 1777 $O
step() { echo; echo "######## $*"; }
inroot() {   # CMD: run in the root with dev/proc/sys bound (maintainer scripts need them)
	for d in dev proc sys; do mountpoint -q $R/$d || mount --bind /$d $R/$d; done
	chroot $R /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive sh -c "$*"
	r=$?
	for d in sys proc dev; do umount $R/$d 2>/dev/null; done
	return $r
}
fresh() { cd $R/home && rm -rf ./desk && cp -a ../etc/skel ./desk && chown -R 1000:1000 ./desk; cd /; }
desk() {   # TAG: desktop up, glxinfo -B, gears 6 s, a screenshot, down
	fresh; sh $UP $R start > $O/$1-start.txt 2>&1; sleep 25
	sh $UP $R run glxinfo -B > $O/$1-glxinfo.txt 2>&1
	grep -E 'OpenGL (renderer|version) string' $O/$1-glxinfo.txt
	sh $UP $R run sh -c "timeout 6 stdbuf -oL es2gears_x11 > /tmp/e2e-$1-gears.txt 2>&1" &
	g=$!; sleep 4
	sh $UP $R run import -window root -resize 50% "png:/tmp/e2e-$1.png"
	wait $g; cp /tmp/e2e-$1-gears.txt /tmp/e2e-$1.png $O/ 2>/dev/null
	echo "gears: $(grep -m1 'frames in' $O/e2e-$1-gears.txt)"
	sh $UP $R stop > /dev/null 2>&1; sleep 2
}

step "0. start from Debian's Mesa only"
inroot "dpkg -s mesa-sgx >/dev/null 2>&1 && apt-get remove -y mesa-sgx; dpkg -s sgx545-firmware >/dev/null 2>&1 && apt-get remove -y sgx545-firmware; true" > $O/0-clean.txt 2>&1
inroot "dpkg -l | grep -E 'mesa-sgx|sgx545-firmware' || echo 'neither installed'"

step "3. firmware: apt install ./sgx545-firmware_*.deb"
cp ${DEBS:-/root/e2e-debs}/*.deb $R/tmp/
inroot "cd /tmp && apt-get install -y ./sgx545-firmware_*.deb" > $O/3-apt.txt 2>&1; echo "apt rc=$?"
tail -3 $O/3-apt.txt
inroot "ls -l /lib/firmware/powervr_sgx/ && md5sum /lib/firmware/powervr_sgx/sgx545_ukernel.bin"
# the box's own kernel loads from the box's /lib/firmware: load the file the package installed
cp $F/sgx545_ukernel.bin $F/sgx545_ukernel-before-e2e.bin
cp $R/lib/firmware/powervr_sgx/sgx545_ukernel.bin $F/sgx545_ukernel.bin
D0=$(dmesg | wc -l)
rmmod powervr_sgx; insmod /root/m3/k/modules/powervr_sgx.ko; sleep 2
echo "-- check it: dmesg | grep powervr_sgx; ls /dev/dri"
dmesg | tail -n +$((D0+1)) | grep powervr_sgx
ls /dev/dri

step "4. Mesa: apt install ./mesa-sgx_*.deb"
inroot "cd /tmp && apt-get install -y ./mesa-sgx_*.deb" > $O/4-apt.txt 2>&1; echo "apt rc=$?"
tail -3 $O/4-apt.txt
inroot "cat /etc/ld.so.conf.d/00-sgx.conf /etc/glvnd/egl_vendor.d/10_sgx.json; ldconfig -p | grep -m2 'libGLX_mesa\|libEGL_mesa'"
echo "-- check it: glxinfo -B (desktop up)"
desk with-sgx

step "rollback: apt remove mesa-sgx"
inroot "apt-get remove -y mesa-sgx" > $O/r-apt.txt 2>&1; echo "apt rc=$?"
tail -2 $O/r-apt.txt
inroot "ls /opt/sgx 2>&1 | head -2; ls /etc/ld.so.conf.d/00-sgx.conf 2>&1; ldconfig -p | grep -m2 'libGLX_mesa\|libEGL_mesa'"
echo "-- Debian's Mesa back: glxinfo -B (desktop up)"
desk after-remove

step "rollback: apt remove sgx545-firmware"
inroot "apt-get remove -y sgx545-firmware" > $O/rf-apt.txt 2>&1; echo "apt rc=$?"
inroot "ls /lib/firmware/powervr_sgx/ 2>&1"
rm -f $R/tmp/*.deb
cp $F/sgx545_ukernel-before-e2e.bin $F/sgx545_ukernel.bin
echo; echo "######## done"
