#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# Builds the Cedarview test stick image, as root, on a Linux CI runner
# (never on the Mac): kit/cedarview/README.md.
#
#   build.sh KERNDIR OUTDIR VERSION [FIRMWARE]
#
# KERNDIR is m2-drm's powervr-sgx-cedarview artifact (bzImage, modroot/, the
# static uAPI tests). FIRMWARE, when given, is an OPEN microkernel image
# (sgx545_ukernel.bin), and a secure-mode one (features 2): the Cedarview
# kernel has no DDK compatibility. Never Intel's. Needs mmdebstrap,
# squashfs-tools, dosfstools, grub-pc-bin, grub-efi-amd64-bin, grub-efi-ia32-bin,
# debian-archive-keyring.
set -eu
K=$(realpath "$1") OUT=$(realpath -m "$2") VER=$3 FW=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
DISKID=5c45c545            # the MBR disk id: root=PARTUUID=$DISKID-02
FAT_MB=64
W=$(mktemp -d)
ROOT=$W/root
mkdir -p "$OUT"

# the root: Debian bookworm, only what the run needs
mmdebstrap --variant=minbase --architectures=amd64 \
	--keyring=/usr/share/keyrings/debian-archive-keyring.gpg \
	--include=kmod,pciutils,usbutils,util-linux,procps,bash,tar,gzip,less \
	--dpkgopt='path-exclude=/usr/share/doc/*' --dpkgopt='path-exclude=/usr/share/man/*' \
	--dpkgopt='path-exclude=/usr/share/info/*' --dpkgopt='path-exclude=/usr/share/locale/*' \
	bookworm "$ROOT" http://deb.debian.org/debian
rm -rf "$ROOT"/var/cache/apt/* "$ROOT"/var/lib/apt/lists/*

KVER=$(ls "$K/modroot/lib/modules")
cp -a "$K/modroot/lib/modules" "$ROOT/lib/"
# modules_install's build/source links arrive as the whole kernel tree
rm -rf "${ROOT:?}/lib/modules/$KVER/build" "${ROOT:?}/lib/modules/$KVER/source"
depmod -b "$ROOT" "$KVER"
install -d "$ROOT/usr/local/lib/kit" "$ROOT/usr/local/sbin" "$ROOT/usr/local/bin"
for t in powervr_sgx_test powervr_sgx_frame powervr_sgx_blit powervr_sgx_hang; do
	[ -f "$K/$t" ] && install -m 0755 "$K/$t" "$ROOT/usr/local/lib/kit/"
done
if [ -n "$FW" ]; then
	install -D -m 0644 "$FW" "$ROOT/lib/firmware/powervr_sgx/sgx545_ukernel.bin"
fi
install -m 0755 "$HERE/kit-init" "$ROOT/sbin/kit-init"
install -m 0755 "$HERE/kit-protect" "$ROOT/usr/local/sbin/"
install -m 0755 "$HERE/kit-stick" "$HERE/kit-run" "$ROOT/usr/local/bin/"
printf 'KIT_VERSION=%s\nDISKID=%s\nKERNEL=%s\n' "$VER" "$DISKID" "$KVER" > "$ROOT/etc/kit-release"
for d in proc sys dev tmp run var/log var/tmp root mnt; do mkdir -p "$ROOT/$d"; done
mksquashfs "$ROOT" "$W/root.sqfs" -comp xz -noappend -quiet

# the image: 1 MiB for GRUB, the FAT partition, the root
SQ_MB=$(( ($(stat -c %s "$W/root.sqfs") + 1048575) / 1048576 + 1 ))
IMG=$W/kit.img
truncate -s $((1 + FAT_MB + SQ_MB))M "$IMG"
sfdisk -q "$IMG" <<EOF
label: dos
label-id: 0x$DISKID
start=1MiB, size=${FAT_MB}MiB, type=c, bootable
type=83
EOF
LOOP=$(losetup -P --show -f "$IMG")
trap 'umount "$W/mnt" 2>/dev/null; losetup -d "$LOOP" 2>/dev/null' EXIT
mkfs.vfat -F 32 -n SGXKIT "${LOOP}p1" >/dev/null
dd if="$W/root.sqfs" of="${LOOP}p2" bs=1M conv=fsync status=none
mkdir -p "$W/mnt"
mount "${LOOP}p1" "$W/mnt"
install -D -m 0644 "$K/bzImage" "$W/mnt/boot/bzImage"
grub-install --target=i386-pc --boot-directory="$W/mnt/boot" --modules="part_msdos fat" "$LOOP"
for t in x86_64-efi i386-efi; do
	grub-install --target=$t --efi-directory="$W/mnt" --boot-directory="$W/mnt/boot" --removable --no-nvram
done
sed "s/@DISKID@/$DISKID/; s/@VERSION@/$VER/" "$HERE/grub.cfg" > "$W/mnt/boot/grub/grub.cfg"
install -m 0644 "$HERE/README-STICK.txt" "$W/mnt/README.txt"
umount "$W/mnt"
losetup -d "$LOOP"
trap - EXIT

NAME=sgx545-cedarview-kit-$VER.img
gzip -9 -n -c "$IMG" > "$OUT/$NAME.gz"
(cd "$OUT" && sha256sum "$NAME.gz" > "$NAME.gz.sha256")
[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:$SUDO_GID" "$OUT"
ls -l "$OUT"
rm -rf "${W:?}"
