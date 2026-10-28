#!/bin/sh
# Pull the SGX545 microkernel firmware out of your own copy of the Cedarview
# DDK 1.7.862890, for re/srvinit/srvinit. Nothing of Imagination's is kept in
# this repo: like openHC's sgx545-um package, we fetch (or reuse) the DDK and
# take what we need from it.
#
# Usage: fetch-firmware.sh [source] [out]
#   source  libsrv_init.so.1.7.862890, a directory holding it (e.g. a DDK
#           sysroot), or cedarview-graphics-drivers_20120717.orig.tar.gz.
#           Default: download the tarball from old-releases.ubuntu.com.
#   out     default: ./sgx545-ukernel-1.7.862890.bin
#
# The firmware file is libsrv_init's .rodata from 0x1e20 to its end (0xef68),
# byte for byte: the five PDS templates SrvInit patches, then the USSE
# microkernel. re/srvinit/SRVINIT.md has the layout.
set -e

LIB=libsrv_init.so.1.7.862890
LIB_SHA=4d3df850191ac363b085afd025982f5eace64977edd5483466649e9effe53b6d
TAR=cedarview-graphics-drivers_20120717.orig.tar.gz
TAR_SHA=c23e7261631860f3bc132c294e269ea982e34e37b5e838fafea0e7070da8fd83
TAR_URL=https://old-releases.ubuntu.com/ubuntu/pool/multiverse/c/cedarview-graphics-drivers/$TAR
FW_OFF=$((0x1e20))
FW_LEN=$((0xd148))
FW_SHA=d7e550e9199e3e4107dc440ce2676ffd1dd3aaf7af4f51a1002e02686bd12861

SRC=$1
OUT=${2:-sgx545-ukernel-1.7.862890.bin}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
check() { [ "$(sha "$1")" = "$2" ] || { echo "$1: sha256 mismatch (want $2)" >&2; exit 1; }; }

if [ -z "$SRC" ]; then
	curl -fsSL -o "$TMP/$TAR" "$TAR_URL"
	SRC=$TMP/$TAR
fi
if [ -d "$SRC" ]; then
	SO=$(find "$SRC" -name $LIB -type f | head -1)
	[ -n "$SO" ] || { echo "no $LIB under $SRC" >&2; exit 1; }
elif [ "${SRC%.tar.gz}" != "$SRC" ]; then
	check "$SRC" $TAR_SHA
	tar -xzf "$SRC" -C "$TMP"
	SO=$(find "$TMP" -name $LIB -type f | head -1)
else
	SO=$SRC
fi
check "$SO" $LIB_SHA

dd if="$SO" of="$OUT" bs=1 skip=$FW_OFF count=$FW_LEN 2>/dev/null
check "$OUT" $FW_SHA
echo "$OUT: $FW_LEN bytes, sha256 $FW_SHA"
