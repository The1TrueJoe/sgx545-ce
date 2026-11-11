#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Lay out an ARMv7 kernel for our OMAP3630 test board (SGX530) with
# powervr_sgx in tree:
#
#   omap3630-kernel.sh <linux source> <drm dir> [extra fragment...]
#
# The config is the board's own (omap3630.config, a 7.1.8 config; olddefconfig
# carries it to a later kernel) plus omap3630.fragment. Built from the same
# source, config and compiler, the modules match the running kernel's symbol versions.
# CROSS_COMPILE defaults to arm-linux-gnueabihf-. Modules that load on the board need
# the board's compiler (gcc 15.3): CI uses Bootlin's, see powervr-sgx-omap3630.yml.
set -eu

SRC=$1
DRM=$2
shift 2
X=${CROSS_COMPILE:-arm-linux-gnueabihf-}

rm -rf "$SRC/drivers/gpu/drm/powervr_sgx"
cp -RL "$DRM/powervr_sgx" "$SRC/drivers/gpu/drm/powervr_sgx"
sh "$DRM/ci/compat.sh" "$SRC" "$SRC/drivers/gpu/drm/powervr_sgx"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Kconfig" ||
	printf '\nsource "drivers/gpu/drm/powervr_sgx/Kconfig"\n' >> "$SRC/drivers/gpu/drm/Kconfig"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Makefile" ||
	printf '\nobj-$(CONFIG_DRM_POWERVR_SGX) += powervr_sgx/\n' >> "$SRC/drivers/gpu/drm/Makefile"

cp "$DRM/ci/omap3630.config" "$SRC/.config"
( cd "$SRC" && ARCH=arm CROSS_COMPILE=$X ./scripts/kconfig/merge_config.sh -m .config \
	"$DRM/ci/omap3630.fragment" "$@" )
make -C "$SRC" ARCH=arm CROSS_COMPILE="$X" olddefconfig

for want in CONFIG_DRM_POWERVR_SGX=m CONFIG_DRM_POWERVR_SGX_OF=y CONFIG_TI_SYSC=y \
	    CONFIG_MODVERSIONS=y CONFIG_PM=y CONFIG_DEBUG_FS=y 'CONFIG_LOCALVERSION=""'; do
	grep -qx "$want" "$SRC/.config" || { echo "omap3630-kernel: $want missing" >&2; exit 1; }
done
