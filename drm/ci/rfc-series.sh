#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Commit the RFC series onto a drm-misc-next tree (a b4 prep branch), as
# upstream would carry it: gma500 0001 and 0002, the uAPI with its docs and
# MAINTAINERS entry, the driver, the KUnit tests. Messages: docs/rfc/series.
#
#   rfc-series.sh <kernel tree> <repo root>
set -eu
K=$1
R=$2
DRM=$R/drm
MSG=$R/docs/rfc/series
D=$K/drivers/gpu/drm/powervr_sgx

cd "$K"
git am -q "$DRM"/gma500/0001-*.patch "$DRM"/gma500/0002-*.patch

# 3: the uAPI, its documentation, MAINTAINERS.
cp "$DRM/powervr_sgx/include/uapi/drm/powervr_sgx_drm.h" include/uapi/drm/
mkdir -p Documentation/gpu/powervr_sgx
cp "$DRM"/Documentation/gpu/powervr_sgx/*.rst Documentation/gpu/powervr_sgx/
grep -q powervr_sgx/index Documentation/gpu/drivers.rst ||
	perl -pi -e 's#^   imagination/index\n#$&   powervr_sgx/index\n#' Documentation/gpu/drivers.rst
grep -q '^DRM DRIVER FOR POWERVR SGX5XX GPUS$' MAINTAINERS ||
	perl -pi -e 'print "DRM DRIVER FOR POWERVR SGX5XX GPUS\nM:\tJoseph Telaak <joseph\@telaak.dev>\nL:\tdri-devel\@lists.freedesktop.org\nS:\tMaintained\nT:\tgit https://gitlab.freedesktop.org/drm/misc/kernel.git\nF:\tDocumentation/gpu/powervr_sgx/\nF:\tdrivers/gpu/drm/powervr_sgx/\nF:\tinclude/uapi/drm/powervr_sgx_drm.h\n\n" if /^DRM DRIVER FOR QEMU.S CIRRUS DEVICE$/' MAINTAINERS
git add include/uapi/drm/powervr_sgx_drm.h Documentation/gpu MAINTAINERS
git commit -q -F "$MSG/03-uapi.txt"

# The upstream Kconfig: no vendor-driver note; KUNIT=0 also drops the KUnit entry.
kconfig() {
	perl -0pe 's/\t  Not compatible with the vendor services driver.*?\n\n//s;
		   s/\n+config DRM_POWERVR_SGX_KUNIT_TEST\n.*\z/\n/s if !$ENV{KUNIT}' "$DRM/powervr_sgx/Kconfig"
}

# 4: the driver, without the KUnit hooks.
rm -rf "$D" && mkdir -p "$D"
for f in "$DRM"/powervr_sgx/*.[ch]; do cp -L "$f" "$D/"; done
perl -0pi -e 's/\n*#if IS_ENABLED\(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST\)\n#include "tests\/[a-z_]+\.c"\n#endif\n/\n/g' "$D"/*.c
KUNIT=0 kconfig > "$D/Kconfig"
# In tree only: no out-of-tree branch, the uAPI header from include/uapi.
perl -0pe 's/# Out of tree:.*?\n//; s/ifneq \(\$\(KERNELRELEASE\),\)\n//; s/ccflags-y \+= .*?\n//;
	   s/\nelse\n.*\z/\n/s' "$DRM/powervr_sgx/Makefile" > "$D/Makefile"
grep -q powervr_sgx drivers/gpu/drm/Kconfig ||
	perl -pi -e 's#^source "drivers/gpu/drm/imagination/Kconfig"\n#$&source "drivers/gpu/drm/powervr_sgx/Kconfig"\n#' drivers/gpu/drm/Kconfig
grep -q powervr_sgx drivers/gpu/drm/Makefile ||
	perl -pi -e 's#^obj-\$\(CONFIG_DRM_POWERVR\) \+= imagination/\n#$&obj-\$(CONFIG_DRM_POWERVR_SGX) += powervr_sgx/\n#' drivers/gpu/drm/Makefile
grep -q powervr_sgx drivers/gpu/drm/Kconfig
grep -q powervr_sgx drivers/gpu/drm/Makefile
git add "$D" drivers/gpu/drm/Kconfig drivers/gpu/drm/Makefile
git commit -q -F "$MSG/04-driver.txt"

# 5: KUnit: the tests, their hooks and the Kconfig entry, as in the repo.
for f in "$DRM"/powervr_sgx/*.c; do
	if grep -q CONFIG_DRM_POWERVR_SGX_KUNIT_TEST "$f"; then cp -L "$f" "$D/"; fi
done
KUNIT=1 kconfig > "$D/Kconfig"
mkdir -p "$D/tests"
cp "$DRM"/powervr_sgx/tests/*.c "$DRM/powervr_sgx/tests/.kunitconfig" "$D/tests/"
git add "$D"
git commit -q -F "$MSG/05-kunit.txt"
git log --oneline -5
