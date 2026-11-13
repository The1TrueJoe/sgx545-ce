#!/bin/sh
# SPDX-License-Identifier: MIT OR Apache-2.0
# Copyright © 2026 Joseph Telaak
#
# Build the sgx-asm host tool ukernel/build.py assembles with (from
# mesa/src/sgx/isa's sgx.xml, without a Mesa build):
#
#   sh ukernel/sgx-asm.sh DIR      -> DIR/sgx-asm, DIR/sgx-disasm
#
# Needs python3 with mako and a C compiler. SGX_MESA_TREE points at an unpacked
# Mesa release of mesa/VERSION; otherwise the release is downloaded into DIR.
set -eu
R=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1"
D=$(cd "$1" && pwd)
. "$R/mesa/VERSION"
T=${SGX_MESA_TREE:-}
if [ -z "$T" ]; then
	[ -f "$D/mesa-src.tar.xz" ] || curl -sfL -o "$D/mesa-src.tar.xz" "$MESA_URL"
	echo "$MESA_SHA256  $D/mesa-src.tar.xz" | sha256sum -c >/dev/null
	[ -d "$D/mesa-$MESA_VERSION" ] || tar -C "$D" -xf "$D/mesa-src.tar.xz"
	T=$D/mesa-$MESA_VERSION
fi
bash "$R/mesa/tests/isa-standalone/standalone.sh" "$T" "$D/build" >"$D/build.log" 2>&1 ||
	{ tail -30 "$D/build.log" >&2; exit 1; }
cp "$D/build/sgx-asm" "$D/build/sgx-disasm" "$D/"
echo "$D/sgx-asm"
