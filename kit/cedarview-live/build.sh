#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# The live Debian test stick (kit/cedarview-live/README.md), as root on a Linux
# CI runner (never on the Mac):
#
#   build.sh DEBS TESTS OUTDIR VERSION [XFERBANK_FW]
#
# DEBS holds the packages a user installs (docs/INSTALL.md): the kernel
# (linux-image-*-sgx), sgx545-firmware and mesa-sgx; the amd64 and all ones go in
# with apt, as the guide says (an i386 mesa-sgx stays out: the root is amd64 only). TESTS holds powervr_sgx_test/_frame/_blit/_hang (static).
# XFERBANK_FW is the open microkernel's blit variant B. Open code only.
# Needs mmdebstrap, squashfs-tools, dosfstools, grub-pc-bin, grub-efi-amd64-bin,
# grub-efi-ia32-bin, and Debian's keyring in KEYRING (a keyring file with trixie's keys).
set -eu
DEBS=$(realpath "$1") TESTS=$(realpath "$2") OUT=$(realpath -m "$3") VER=$4 FWX=${5:-}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
KEYRING=${KEYRING:?a Debian archive keyring file with the trixie keys}
W=$(mktemp -d)
ROOT=$W/root
mkdir -p "$OUT"

pkgs=$(cat "$REPO/mesa/tests/desktop/packages.txt" "$HERE/packages.txt" | grep -v '^#' | grep . | sort -u | paste -sd, -)
mmdebstrap --variant=apt --architectures=amd64 --keyring="$KEYRING" \
	--include="$pkgs" \
	--dpkgopt='path-exclude=/usr/share/doc/*' --dpkgopt='path-exclude=/usr/share/man/*' \
	--dpkgopt='path-exclude=/usr/share/info/*' \
	--customize-hook='chroot "$1" useradd -m -u 1000 -s /bin/bash -G video,input,render,audio desk' \
	--customize-hook="mkdir -p \"\$1/tmp/kitdebs\" && cp $DEBS/*_amd64.deb $DEBS/*_all.deb \"\$1/tmp/kitdebs/\"" \
	--customize-hook='chroot "$1" sh -c "apt-get install -y /tmp/kitdebs/*.deb && rm -rf /tmp/kitdebs"' \
	--customize-hook='rm -rf "$1"/var/cache/apt/* "$1"/var/lib/apt/lists/*' \
	trixie "$ROOT" http://deb.debian.org/debian

# the kit
install -d "$ROOT/usr/local/lib/kit" "$ROOT/usr/local/share/kit"
for t in powervr_sgx_test powervr_sgx_frame powervr_sgx_blit powervr_sgx_hang; do
	install -m 0755 "$TESTS/$t" "$ROOT/usr/local/lib/kit/"
done
[ -n "$FWX" ] && install -m 0644 "$FWX" "$ROOT/usr/local/lib/kit/sgx545_ukernel-xferbank.bin"
install -m 0755 "$HERE/kit-live-run" "$ROOT/usr/local/sbin/"
install -m 0755 "$HERE/kit-xvariant" "$HERE/kit-session" "$HERE/kit-smoke" "$HERE/kit-xorg" \
	"$HERE/kit-display" "$HERE/kit-save" "$HERE/kit-hangcap" "$HERE/kit-wdt" "$ROOT/usr/local/bin/"
install -m 0644 "$HERE/desktop-variants" "$ROOT/usr/local/lib/kit/"
install -m 0644 "$HERE/test.html" "$ROOT/usr/local/share/kit/"
install -m 0644 "$HERE/99-kit-readonly.rules" "$ROOT/etc/udev/rules.d/"
install -m 0755 "$HERE/initramfs-hook" "$ROOT/etc/initramfs-tools/hooks/kit-readonly"
install -m 0644 "$HERE/kit.service" "$ROOT/etc/systemd/system/"
ln -sf /etc/systemd/system/kit.service "$ROOT/etc/systemd/system/multi-user.target.wants/kit.service"
ln -sf /lib/systemd/system/multi-user.target "$ROOT/etc/systemd/system/default.target"
printf 'KIT_SERIES=%s\nKIT_VERSION=%s\n' "$(cat "$HERE/SERIES")" "$VER" > "$ROOT/etc/kit-release"
echo cedarview-kit > "$ROOT/etc/hostname"
# the initramfs again, now with live-boot's and the read-only rule's hooks
chroot "$ROOT" update-initramfs -u -k all

KVER=$(ls "$ROOT/lib/modules" | grep -- -sgx | head -1)
[ -n "$KVER" ] || { echo "build.sh: no -sgx kernel in the root" >&2; exit 1; }
mksquashfs "$ROOT" "$W/filesystem.squashfs" -comp xz -noappend -quiet -e boot

SQ_MB=$(( ($(stat -c %s "$W/filesystem.squashfs") + 1048575) / 1048576 ))
FAT_MB=$((SQ_MB + 200))
IMG=$W/kit.img
truncate -s $((1 + FAT_MB))M "$IMG"
sfdisk -q "$IMG" <<EOF
label: dos
start=1MiB, type=c, bootable
EOF
LOOP=$(losetup -P --show -f "$IMG")
trap 'umount "$W/mnt" 2>/dev/null; losetup -d "$LOOP" 2>/dev/null' EXIT
mkfs.vfat -F 32 -n SGXKIT "${LOOP}p1" >/dev/null
mkdir -p "$W/mnt"
mount "${LOOP}p1" "$W/mnt"
install -d "$W/mnt/live"
cp "$ROOT/boot/vmlinuz-$KVER" "$W/mnt/live/vmlinuz"
cp "$ROOT/boot/initrd.img-$KVER" "$W/mnt/live/initrd.img"
cp "$W/filesystem.squashfs" "$W/mnt/live/"
grub-install --target=i386-pc --boot-directory="$W/mnt/boot" --modules="part_msdos fat" "$LOOP"
for t in x86_64-efi i386-efi; do
	grub-install --target=$t --efi-directory="$W/mnt" --boot-directory="$W/mnt/boot" --removable --no-nvram
done
sed "s/@VERSION@/$(cat "$HERE/SERIES")/" "$HERE/grub.cfg" > "$W/mnt/boot/grub/grub.cfg"
install -m 0644 "$HERE/README-STICK.txt" "$W/mnt/README.txt"
umount "$W/mnt"
losetup -d "$LOOP"
trap - EXIT

NAME=sgx545-cedarview-kit-$(cat "$HERE/SERIES")-$VER.img
gzip -6 -n -c "$IMG" > "$OUT/$NAME.gz"
(cd "$OUT" && sha256sum "$NAME.gz" > "$NAME.gz.sha256")
[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:$SUDO_GID" "$OUT"
ls -l "$OUT"
rm -rf "${W:?}"
