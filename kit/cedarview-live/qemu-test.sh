#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# CI check of the live stick in QEMU (no SGX there: the GPU steps fail or skip,
# the desktop runs on softpipe):
#   qemu-test.sh IMAGE.img.gz bios|uefi
# Boots it as a removable USB stick next to an AHCI disk with known contents.
# Passes when the run leaves a results file on SGXKIT with the desktop's
# screenshots, and the disk is unchanged. Needs qemu-system-x86, mtools, ovmf.
set -eu
GZ=$1 FW=$2
W=$(mktemp -d)
gunzip -c "$GZ" > "$W/stick.img"
M="$W/stick.img@@1M"
mcopy -n -i "$M" ::/boot/grub/grub.cfg "$W/grub.cfg"
sed -i 's/ systemd.show_status=0$/ systemd.show_status=0 console=tty0 console=ttyS0 kit.poweroff=1/; s/^set timeout=.*/set timeout=0/' "$W/grub.cfg"
grep -q kit.poweroff=1 "$W/grub.cfg"
mcopy -o -i "$M" "$W/grub.cfg" ::/boot/grub/grub.cfg
head -c 64M /dev/urandom > "$W/disk.img"
before=$(sha256sum < "$W/disk.img")
case $FW in
bios) fwopt= ;;
uefi) cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
	fwopt="-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=$W/vars.fd" ;;
*) echo "bios or uefi" >&2; exit 2 ;;
esac
accel=tcg; [ -w /dev/kvm ] && accel=kvm
# shellcheck disable=SC2086
timeout 2400 qemu-system-x86_64 -machine q35,accel=$accel -m 2048 -smp 2 -vga std -display none \
	-serial file:"$W/serial.log" $fwopt -no-reboot \
	-device qemu-xhci,id=xhci \
	-drive if=none,id=stick,format=raw,file="$W/stick.img" -device usb-storage,bus=xhci.0,drive=stick,removable=on,bootindex=0 \
	-drive if=none,id=disk,format=raw,file="$W/disk.img" -device ide-hd,drive=disk,bus=ide.0 || true
tail -30 "$W/serial.log"
after=$(sha256sum < "$W/disk.img")
[ "$before" = "$after" ] || { echo "qemu-test: the internal disk CHANGED" >&2; exit 1; }
f=$(mdir -b -i "$M" ::/ | grep -E 'results-kit.*\.tar\.gz$' | head -1)
[ -n "$f" ] || { echo "qemu-test: no results file on SGXKIT" >&2; exit 1; }
mcopy -n -i "$M" "$f" "$W/r.tgz"
tar -tzf "$W/r.tgz" | sort
tar -xzOf "$W/r.tgz" results/summary.txt
tar -tzf "$W/r.tgz" | grep -q 'results/screenshots/01-desktop.png' || { echo "qemu-test: no desktop screenshot" >&2; exit 1; }
echo "qemu-test $FW: ok, $(basename "$f"), disk unchanged"
rm -rf "$W"
