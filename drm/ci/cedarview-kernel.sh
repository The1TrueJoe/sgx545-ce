#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Lay out a Cedarview (Atom D2000/N2000, GMA3600/3650) test kernel: mainline
# with drm/gma500/*.patch (the SGX as an auxiliary device) and powervr_sgx in
# tree, from x86_64_defconfig plus cedarview.fragment.
#
#   cedarview-kernel.sh <linux source> <drm dir> [extra fragment...]
set -eu

SRC=$1
DRM=$2
shift 2

for p in "$DRM"/gma500/*.patch; do
	echo ">> patch $(basename "$p")"
	patch -d "$SRC" -p1 --forward --batch < "$p"
done

rm -rf "$SRC/drivers/gpu/drm/powervr_sgx"
cp -RL "$DRM/powervr_sgx" "$SRC/drivers/gpu/drm/powervr_sgx"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Kconfig" ||
	printf '\nsource "drivers/gpu/drm/powervr_sgx/Kconfig"\n' >> "$SRC/drivers/gpu/drm/Kconfig"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Makefile" ||
	printf '\nobj-$(CONFIG_DRM_POWERVR_SGX) += powervr_sgx/\n' >> "$SRC/drivers/gpu/drm/Makefile"

make -C "$SRC" ARCH=x86_64 x86_64_defconfig
( cd "$SRC" && ARCH=x86_64 ./scripts/kconfig/merge_config.sh -m .config "$DRM/ci/cedarview.fragment" "$@" )
make -C "$SRC" ARCH=x86_64 olddefconfig

for want in CONFIG_DRM_GMA500=m CONFIG_DRM_POWERVR_SGX=m CONFIG_DRM_POWERVR_SGX_GMA500=y \
	    CONFIG_AUXILIARY_BUS=y CONFIG_MODULES=y; do
	grep -qx "$want" "$SRC/.config" || { echo "cedarview-kernel: $want missing" >&2; exit 1; }
done
