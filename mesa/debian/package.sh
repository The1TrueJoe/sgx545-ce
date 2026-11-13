#!/bin/sh
# Pack a `mesa/build.sh debian` install as mesa-sgx for one architecture:
#
#   mesa/debian/package.sh <install dir> <amd64|i386> <version>
#
# amd64 lives in /opt/sgx/lib, i386 in /opt/sgx/lib/i386-linux-gnu (SGX_MESA_LIBDIR
# at build time). Each architecture brings its own ld.so.conf.d entry and glvnd
# EGL vendor file (mesa/debian/etc.<arch>), so the two packages share only
# /opt/sgx/share, which is the same in both: Multi-Arch: same, co-installable.
# MESA_SGX_BREAKS, when set, is the control file's Breaks (a release's: the kernel
# packages too old for its uAPI). Writes mesa-sgx_<version>_<arch>.deb here.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
INSTALL=$1 ARCH=$2 VER=$3
case $ARCH in
amd64) L=lib ;;
i386) L=lib/i386-linux-gnu ;;
*) echo "package.sh: no layout for $ARCH" >&2; exit 1 ;;
esac
test -e "$INSTALL/opt/sgx/$L/dri/gma500_dri.so"

P=$(mktemp -d)/mesa-sgx
mkdir -p "$P/DEBIAN"
cp -a "$INSTALL/opt" "$P/"
cp -a "$HERE/etc.$ARCH" "$P/etc"
rm -rf "$P/opt/sgx/include" "$P/opt/sgx/$L/pkgconfig"
cat > "$P/DEBIAN/control" <<CTL
Package: mesa-sgx
Version: $VER
Architecture: $ARCH
Multi-Arch: same
Maintainer: Joseph Telaak <joseph@telaak.dev>
Depends: libc6, libllvm19, libelf1t64, libdrm2, libglvnd0, libgl1, libegl1, libgles2, libx11-6, libx11-xcb1, libxcb1, libxcb-dri3-0, libxcb-present0, libxcb-sync1, libxcb-xfixes0, libxcb-randr0, libxshmfence1, libxxf86vm1, libwayland-client0, libwayland-server0, libexpat1, zlib1g, libzstd1
${MESA_SGX_BREAKS:+Breaks: $MESA_SGX_BREAKS
}Section: libs
Priority: optional
Description: Mesa with the sgx driver for the PowerVR SGX545
 OpenGL, OpenGL ES, EGL and GBM for the PowerVR SGX545 of Intel's
 Cedarview and CE5300 SoCs, under the powervr_sgx kernel driver.
 Installed in /opt/sgx and used ahead of Debian's Mesa. The i386 package
 is for 32-bit programs (games) on a 64-bit system, beside the amd64 one.
CTL
printf '#!/bin/sh\nset -e\nldconfig\n' > "$P/DEBIAN/postinst"
cp "$P/DEBIAN/postinst" "$P/DEBIAN/postrm"
chmod 755 "$P/DEBIAN/postinst" "$P/DEBIAN/postrm"
dpkg-deb --root-owner-group --build "$P" "mesa-sgx_${VER}_${ARCH}.deb"
rm -rf "$(dirname "$P")"
