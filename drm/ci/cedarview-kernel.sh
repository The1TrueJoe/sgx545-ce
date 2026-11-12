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

# KIT=1: also the test-kit-only patches (drm/ci/kit), never in the upstream check.
for p in "$DRM"/shmem/*.patch "$DRM"/gma500/*.patch $([ "${KIT:-0}" = 1 ] && echo "$DRM"/ci/kit/*.patch); do
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

make -C "$SRC" ARCH=x86_64 x86_64_defconfig
( cd "$SRC" && ARCH=x86_64 ./scripts/kconfig/merge_config.sh -m .config "$DRM/ci/cedarview.fragment" "$@" )
make -C "$SRC" ARCH=x86_64 olddefconfig

for want in CONFIG_DRM_GMA500=m CONFIG_DRM_POWERVR_SGX=m CONFIG_DRM_POWERVR_SGX_GMA500=y \
	    CONFIG_AUXILIARY_BUS=y CONFIG_MODULES=y CONFIG_USB_STORAGE=y CONFIG_SQUASHFS=y \
	    CONFIG_OVERLAY_FS=y CONFIG_DRM_SIMPLEDRM=y CONFIG_FRAMEBUFFER_CONSOLE=y; do
	grep -qx "$want" "$SRC/.config" || { echo "cedarview-kernel: $want missing" >&2; exit 1; }
done
if grep -q '^CONFIG_HIBERNATION=y' "$SRC/.config"; then
	echo "cedarview-kernel: hibernation must stay off (the kit never resumes from a disk)" >&2
	exit 1
fi
