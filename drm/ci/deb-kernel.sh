#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Lay out a Debian-installable kernel with gma500's SGX auxiliary device and
# powervr_sgx in tree, from Debian's own amd64 config (the config file of
# linux-image-*-amd64-unsigned from DEB_SUITE, default trixie) plus
# deb.fragment:
#
#   deb-kernel.sh <linux source> <drm dir>
set -eu
SRC=$1
DRM=$2
SUITE=${DEB_SUITE:-trixie}
MIRROR=http://deb.debian.org/debian

for p in "$DRM"/gma500/*.patch; do
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

( cd "$SRC" && ARCH=x86_64 ./scripts/kconfig/merge_config.sh -m .config "$DRM/ci/deb.fragment" )
make -C "$SRC" ARCH=x86_64 olddefconfig
for want in CONFIG_DRM_GMA500=m CONFIG_DRM_POWERVR_SGX=m CONFIG_DRM_POWERVR_SGX_GMA500=y; do
	grep -qx "$want" "$SRC/.config" || { echo "deb-kernel: $want missing" >&2; exit 1; }
done
