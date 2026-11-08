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
# tree, then the config from x86_64_defconfig, the fragments build.sh composes
# for the board and real-kernel.fragment. The fragment list and the DT files
# come from build.sh itself (OHC_DEFCONFIG_ONLY), so the board/ea/<model>/<board>
# layers are followed as openHC follows them. On top: the vendor driver
# (SGX545_CE) out and powervr_sgx in as a module (powervr-sgx.fragment).
# BOARD picks the board (default ea1-v1); EXTRA_FRAGMENTS adds fragments last
# (debug.fragment for the lockdep/KASAN/UBSAN kernel).
set -eu

OHC=$1
SRC=$2
DRM=$3
BOARD=${BOARD:-ea1-v1}
B=$OHC/board

for p in "$B"/common/patches/linux/*.patch "$B"/ea/common/patches/linux/*.patch; do
	[ -e "$p" ] || continue
	echo ">> patch $(basename "$p")"
	patch -d "$SRC" -p1 --forward --batch < "$p"
done

for m in "$B/common/kernel" "$B/ea/common/kernel"; do
	for t in arch drivers include sound; do
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

# The board's composition, as build.sh resolves it.
GEN=$(mktemp -d)
EFF=$(cd "$OHC" && OHC_DEFCONFIG_ONLY=1 BOARD=$BOARD BR2_OUTPUT_DIR=$GEN bash build/build.sh |
	sed -n '/^BR2_/p')
val() { printf '%s\n' "$EFF" | sed -n "s/^$1=\"\(.*\)\"$/\1/p" | tail -1 | sed "s|\$(BR2_EXTERNAL_OPENHC_PATH)|$B|g"; }
for f in $(val BR2_OHC_BOARD_DTS); do
	install -D -m644 "$B/$f" "$SRC/arch/x86/boot/dts/$(basename "$f")"
done

rm -rf "$SRC/drivers/gpu/drm/powervr_sgx"
cp -RL "$DRM/powervr_sgx" "$SRC/drivers/gpu/drm/powervr_sgx"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Kconfig" ||
	printf '\nsource "drivers/gpu/drm/powervr_sgx/Kconfig"\n' >> "$SRC/drivers/gpu/drm/Kconfig"
grep -q powervr_sgx "$SRC/drivers/gpu/drm/Makefile" ||
	printf '\nobj-$(CONFIG_DRM_POWERVR_SGX) += powervr_sgx/\n' >> "$SRC/drivers/gpu/drm/Makefile"

frags="$(val BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES)"
[ -n "$frags" ] || { echo "openhc-ea-kernel: build.sh gave no fragments for $BOARD" >&2; exit 1; }
frags="$frags $B/ea/common/linux/real-kernel.fragment $DRM/ci/powervr-sgx.fragment ${EXTRA_FRAGMENTS:-}"
echo ">> fragments:"
printf '   %s\n' $frags

make -C "$SRC" ARCH=x86_64 x86_64_defconfig
( cd "$SRC" && ARCH=x86_64 ./scripts/kconfig/merge_config.sh -m .config $frags )
make -C "$SRC" ARCH=x86_64 olddefconfig
"$SRC/scripts/config" --file "$SRC/.config" -d MODULE_SIG -d MODULE_SIG_ALL -d MODULE_SIG_FORCE
make -C "$SRC" ARCH=x86_64 olddefconfig

for want in CONFIG_DRM=y CONFIG_DRM_POWERVR_SGX=m CONFIG_MODULES=y CONFIG_DEV_COREDUMP=y CONFIG_DRM_SIMPLEDRM=y; do
	grep -qx "$want" "$SRC/.config" || { echo "openhc-ea-kernel: $want missing" >&2; exit 1; }
done
if grep -q '^CONFIG_SGX545_CE=' "$SRC/.config"; then
	echo "openhc-ea-kernel: the vendor driver is still configured" >&2
	exit 1
fi
