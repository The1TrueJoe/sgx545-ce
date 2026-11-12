#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# CI check of the live stick in QEMU (no SGX there: the GPU steps fail or skip,
# the desktop runs on llvmpipe):
#   qemu-test.sh IMAGE.img.gz bios|uefi [xhang]
# Boots it as a removable USB stick next to an AHCI disk with known contents.
# Passes when the run turns itself off, leaves one complete results file on
# SGXKIT with the desktop's screenshots, and the disk is unchanged. xhang adds
# kit.test=xhang: X stops dead in variant A, and the run must capture it, stop
# it and carry on with the next variant. Needs qemu-system-x86, mtools, ovmf.
set -eu
GZ=$1 FW=$2 MODE=${3:-}
W=$(mktemp -d)
gunzip -c "$GZ" > "$W/stick.img"
M="$W/stick.img@@1M"
mcopy -n -i "$M" ::/boot/grub/grub.cfg "$W/grub.cfg"
extra=; [ "$MODE" = xhang ] && extra=" kit.test=xhang"
sed -i "s/ systemd.show_status=0\$/ systemd.show_status=0 console=tty0 console=ttyS0 kit.poweroff=1$extra/; s/^set timeout=.*/set timeout=0/" "$W/grub.cfg"
grep -q "console=ttyS0 kit.poweroff=1$extra" "$W/grub.cfg"
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
grep -q 'kit: saved=yes' "$W/serial.log" || { echo "qemu-test: the run did not finish and save" >&2; exit 1; }
mcopy -n -i "$M" ::/kit-progress.txt "$W/progress.txt" && grep -q '^complete' "$W/progress.txt" ||
	{ echo "qemu-test: the progress marker is not complete" >&2; exit 1; }
if [ "$MODE" = xhang ]; then
	s=$(tar -xzOf "$W/r.tgz" results/summary.txt)
	echo "$s" | grep -q 'FAIL  \[A\] desktop variant.*no desktop within 90 s' || { echo "qemu-test: the watchdog did not stop variant A" >&2; exit 1; }
	tar -tzf "$W/r.tgz" | grep -q 'results/desk-A/hang.txt' || { echo "qemu-test: no hang capture for variant A" >&2; exit 1; }
	tar -tzf "$W/r.tgz" | grep -qE 'results/desk-[^A]/screenshots/01-desktop.png' || { echo "qemu-test: no later variant reached its desktop after the hang" >&2; exit 1; }
else
	tar -tzf "$W/r.tgz" | grep -q 'results/desk-A/screenshots/01-desktop.png' || { echo "qemu-test: no desktop screenshot" >&2; exit 1; }
fi
echo "qemu-test $FW${MODE:+ $MODE}: ok, $(basename "$f"), disk unchanged"
rm -rf "$W"
