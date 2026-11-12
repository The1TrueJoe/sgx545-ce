#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Lay out a Debian-installable kernel with gma500's SGX auxiliary device and
# powervr_sgx in tree, from Debian's own amd64 config (the config file of
# linux-image-*-amd64-unsigned from DEB_SUITE, default trixie) plus
# deb.fragment, then EXTRA_FRAGMENTS (lockdep.fragment for the lockdep flavor).
# FLAVOR release: what people install. kit: also the test kit's gma500 patches
# (drm/ci/kit: the display register dump, the self-refresh and FIFO switches),
# for the kit's sticks only.
#
#   deb-kernel.sh <linux source> <drm dir> release|kit
set -eu
SRC=$1
DRM=$2
FLAVOR=${3:-}
SUITE=${DEB_SUITE:-trixie}
MIRROR=http://deb.debian.org/debian

case $FLAVOR in
release) kit= ;;
kit) kit=$(ls "$DRM"/ci/kit/*.patch) ;;
*) echo "deb-kernel: flavor release or kit, not '$FLAVOR'" >&2; exit 2 ;;
esac
for p in "$DRM"/shmem/*.patch "$DRM"/gma500/*.patch $kit; do
	echo ">> patch $(basename "$p")"
	patch -d "$SRC" -p1 --forward --batch < "$p"
done
rm -rf "$SRC/drivers/gpu/drm/powervr_sgx"
cp -RL "$DRM/powervr_sgx" "$SRC/drivers/gpu/drm/powervr_sgx"
sh "$DRM/ci/compat.sh" "$SRC" "$SRC/drivers/gpu/drm/powervr_sgx"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Kconfig" ||
	printf '\nsource "drivers/gpu/drm/powervr_sgx/Kconfig"\n' >> "$SRC/drivers/gpu/drm/Kconfig"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Makefile" ||
	printf '\nobj-$(CONFIG_DRM_POWERVR_SGX) += powervr_sgx/\n' >> "$SRC/drivers/gpu/drm/Makefile"

# Debian's config, from its kernel package.
T=$(mktemp -d)
pkg=$(curl -sf "$MIRROR/dists/$SUITE/main/binary-amd64/Packages.xz" | xz -dc |
	sed -n 's/^Filename: \(.*linux-image-[0-9.]*+deb[0-9]*-amd64-unsigned_.*\.deb\)$/\1/p' | sort -V | tail -1)
[ -n "$pkg" ] || { echo "deb-kernel: no amd64 kernel package in $SUITE" >&2; exit 1; }
echo ">> Debian config from $pkg"
curl -sfo "$T/k.deb" "$MIRROR/$pkg"
( cd "$T" && ar x k.deb && tar xf data.tar.* ./boot )
cp "$T"/boot/config-* "$SRC/.config"
rm -rf "$T"

( cd "$SRC" && ARCH=x86_64 ./scripts/kconfig/merge_config.sh -m .config "$DRM/ci/deb.fragment" ${EXTRA_FRAGMENTS:-} )
make -C "$SRC" ARCH=x86_64 olddefconfig
for want in CONFIG_DRM_GMA500=m CONFIG_DRM_POWERVR_SGX=m CONFIG_DRM_POWERVR_SGX_GMA500=y; do
	grep -qx "$want" "$SRC/.config" || { echo "deb-kernel: $want missing" >&2; exit 1; }
done
