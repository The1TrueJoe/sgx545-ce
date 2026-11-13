#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Make a DKMS source tree for powervr_sgx:
#
#   mkdkms.sh [--cedarview] VERSION [OUT]     -> OUT/powervr_sgx-VERSION (default /usr/src)
#
# then: dkms add powervr_sgx/VERSION && dkms install powervr_sgx/VERSION.
# The tree has the driver's sources with the shared stream files copied in
# (they are symlinks in a checkout), the uAPI header under include/, and
# gma500's include/drm/gma500_sgx.h, which drm/gma500/0001 adds to a kernel.
#
# --cedarview builds the binding to gma500's auxiliary device: the kernel
# needs gma500 with drm/gma500/0001 (and 0002 for PRIME), built in or as a
# module of its own. The Kconfig symbol goes in as a make variable (for the
# Makefile) and as a define (for IS_ENABLED() in the code).
set -eu
OPTIONS=
if [ "${1:-}" = --cedarview ]; then
	OPTIONS=" CONFIG_DRM_POWERVR_SGX_GMA500=y KCFLAGS=-DCONFIG_DRM_POWERVR_SGX_GMA500=1"
	shift
fi
VER=$1
OUT=${2:-/usr/src}
HERE=$(cd "$(dirname "$0")" && pwd)
DST=$OUT/powervr_sgx-$VER
rm -rf "$DST"
mkdir -p "$DST/include/drm"
cp -RL "$HERE/../powervr_sgx/." "$DST/"
rm -rf "$DST/tests"
cp "$HERE/../ci/compat.sh" "$HERE/../ci/compat-gem-gpuva.patch" "$DST/"
# The header as gma500 0001 creates it: that hunk's added lines.
awk '/^\+\+\+ b\/include\/drm\/gma500_sgx.h$/ { on = 1; next }
     on && /^diff --git / { exit }
     on && /^@@/ { next }
     on && /^\+/ { print substr($0, 2) }' \
	"$HERE"/../gma500/0001-*.patch > "$DST/include/drm/gma500_sgx.h"
[ -s "$DST/include/drm/gma500_sgx.h" ]
sed "s/@VERSION@/$VER/; s/@OPTIONS@/$OPTIONS/" "$HERE/dkms.conf" > "$DST/dkms.conf"
echo "$DST"
