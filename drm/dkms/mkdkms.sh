#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Make a DKMS source tree for powervr_sgx:
#
#   mkdkms.sh VERSION [OUT]     -> OUT/powervr_sgx-VERSION (default /usr/src)
#
# then: dkms add powervr_sgx/VERSION && dkms install powervr_sgx/VERSION.
# The tree has the driver's sources with the shared stream files copied in
# (they are symlinks in a checkout) and the uAPI header under include/.
set -eu
VER=$1
OUT=${2:-/usr/src}
HERE=$(cd "$(dirname "$0")" && pwd)
DST=$OUT/powervr_sgx-$VER
rm -rf "$DST"
mkdir -p "$DST"
cp -RL "$HERE/../powervr_sgx/." "$DST/"
rm -rf "$DST/tests"
cp "$HERE/../ci/compat.sh" "$HERE/../ci/compat-gem-gpuva.patch" "$DST/"
sed "s/@VERSION@/$VER/" "$HERE/dkms.conf" > "$DST/dkms.conf"
echo "$DST"
