#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Lay out openHC's EA "real" kernel outside Buildroot, with powervr_sgx in tree:
#
#   openhc-ea-kernel.sh <openHC checkout> <linux source> <drm dir>
#
# Does what openHC's build does to the kernel (board/external.mk and
# build/build.sh): global patches in Buildroot's order, the board/common and
# board/ea/common kernel mirrors with their objs.mk lines, the board's device
# tree, then the config from x86_64_defconfig, the family and board fragments,
# the feature fragments and real-kernel.fragment. On top: the vendor driver
# (SGX545_CE) out and powervr_sgx in as a module (powervr-sgx.fragment).
# BOARD picks the board (default ea1-v1).
set -eu

OHC=$1
SRC=$2
DRM=$3
BOARD=${BOARD:-ea1-v1}
B=$OHC/board
BD=$B/ea/$BOARD

for p in "$B"/common/patches/linux/*.patch "$B"/ea/common/patches/linux/*.patch; do
	[ -e "$p" ] || continue
	echo ">> patch $(basename "$p")"
	patch -d "$SRC" -p1 --forward --batch < "$p"
done

for m in "$B/common/kernel" "$B/ea/common/kernel"; do
	for t in arch drivers sound; do
		[ -d "$m/$t" ] || continue
		( cd "$m" && find "$t" \( -name '*.c' -o -name '*.h' -o -name '*.dts' -o -name '*.dtsi' \
			-o -name Makefile -o -name Kbuild -o -name Kconfig \) ) | while read -r f; do
			install -D -m644 "$m/$f" "$SRC/$f"
		done
	done
	# The vendor driver's lines are left out: it is not in this tree.
	grep -v '^[[:space:]]*#' "$m/objs.mk" | grep '|' | grep -v sgx545ce |
	while IFS='|' read -r mk line; do
		obj=${line##*+= }
		grep -q "$obj" "$SRC/$mk" || printf '\n%s\n' "$line" >> "$SRC/$mk"
	done
done

install -D -m644 "$BD/$BOARD.dts" "$SRC/arch/x86/boot/dts/$BOARD.dts"

rm -rf "$SRC/drivers/gpu/drm/powervr_sgx"
cp -RL "$DRM/powervr_sgx" "$SRC/drivers/gpu/drm/powervr_sgx"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Kconfig" ||
	printf '\nsource "drivers/gpu/drm/powervr_sgx/Kconfig"\n' >> "$SRC/drivers/gpu/drm/Kconfig"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Makefile" ||
	printf '\nobj-$(CONFIG_DRM_POWERVR_SGX) += powervr_sgx/\n' >> "$SRC/drivers/gpu/drm/Makefile"

features=$(cat "$B/ea/common/ohc.features" "$BD/ohc.features" 2>/dev/null |
	sed 's/#.*//' | tr ' ' '\n' | awk 'NF && !seen[$0]++')
frags="$B/ea/common/linux/common.fragment"
[ -f "$BD/linux/$BOARD.fragment" ] && frags="$frags $BD/linux/$BOARD.fragment"
for f in $features; do
	for d in "$B/common/features/$f" "$B/ea/common/features/$f" "$BD/features/$f"; do
		[ -f "$d/linux.fragment" ] && frags="$frags $d/linux.fragment"
	done
done
frags="$frags $B/ea/common/linux/real-kernel.fragment $DRM/ci/powervr-sgx.fragment"
echo ">> fragments:"
printf '   %s\n' $frags

make -C "$SRC" ARCH=x86_64 x86_64_defconfig
( cd "$SRC" && ARCH=x86_64 ./scripts/kconfig/merge_config.sh -m .config $frags )
make -C "$SRC" ARCH=x86_64 olddefconfig
"$SRC/scripts/config" --file "$SRC/.config" -d MODULE_SIG -d MODULE_SIG_ALL -d MODULE_SIG_FORCE
make -C "$SRC" ARCH=x86_64 olddefconfig

for want in CONFIG_DRM=y CONFIG_DRM_POWERVR_SGX=m CONFIG_MODULES=y CONFIG_DEV_COREDUMP=y; do
	grep -qx "$want" "$SRC/.config" || { echo "openhc-ea-kernel: $want missing" >&2; exit 1; }
done
if grep -q '^CONFIG_SGX545_CE=' "$SRC/.config"; then
	echo "openhc-ea-kernel: the vendor driver is still configured" >&2
	exit 1
fi
