#!/bin/sh
# Build Mesa with the sgx545 gallium driver and the EGL fbdev platform.
#
#   mesa/build.sh [x86_64] [i386]       (default: both)
#
# Fetches the release in mesa/VERSION (checked against its sha256), unpacks
# it under $SGX545_MESA_BUILD (default build/mesa in the repo), applies
# mesa/patches, copies the mesa/src overlay over the tree, and builds with
# zig cc for each target:
#   x86_64  glibc 2.36 ABI: openHC's Buildroot userspace (glibc 2.42) runs it;
#   i386    glibc 2.36: the Debian bookworm i386 root the Minecraft runtime uses.
# Installs into $SGX545_MESA_BUILD/<arch>/install (lib/libEGL.so.1,
# lib/libGLESv2.so.2, lib/libgallium-<ver>.so, ...), and copies the winsys
# test (sgx545_frame_test) next to it.
#
# Needs: zig (0.14+), meson, ninja, python3 with mako and pyyaml, bison 3,
# flex, network for the tarball and the libdrm wrap.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
. "$HERE/VERSION"
B=${SGX545_MESA_BUILD:-$REPO/build/mesa}
TREE=$B/mesa-$MESA_VERSION
TARBALL=$B/mesa-$MESA_VERSION.tar.xz
ARCHES=${*:-x86_64 i386}

mkdir -p "$B"
if [ ! -f "$TARBALL" ]; then
	curl -fL -o "$TARBALL.part" "$MESA_URL"
	mv "$TARBALL.part" "$TARBALL"
fi
echo "$MESA_SHA256  $TARBALL" | shasum -a 256 -c - >/dev/null || {
	echo "build.sh: $TARBALL does not match mesa/VERSION's sha256" >&2; exit 1; }

if [ ! -f "$TREE/.sgx545-patched" ]; then
	rm -rf "$TREE"
	tar -xJf "$TARBALL" -C "$B"
	for p in "$HERE"/patches/*.patch; do
		patch -d "$TREE" -p1 --batch --forward < "$p"
	done
	touch "$TREE/.sgx545-patched"
fi
# the overlay, every build
(cd "$HERE/src" && tar cf - .) | (cd "$TREE/src" && tar xf -)
ln -sfn "$REPO/kernel/src" "$TREE/include/sgx545-kernel"
ln -sfn "$REPO/libpvr" "$TREE/src/gallium/winsys/sgx545/drm/libpvr"
python3 "$REPO/tools/gen/mkusse.py" > "$TREE/src/gallium/winsys/sgx545/drm/sgx545_usse_progs.h"

for arch in $ARCHES; do
	case $arch in
	x86_64) target=x86_64-linux-gnu.2.36; family=x86_64; cpu=x86_64 ;;
	i386) target=x86-linux-gnu.2.36; family=x86; cpu=i686 ;;
	*) echo "build.sh: unknown arch $arch" >&2; exit 1 ;;
	esac
	O=$B/$arch
	mkdir -p "$O"
	sed -e "s|@ZIGCC@|$HERE/cross/zigcc|" -e "s|@TARGET@|$target|" -e "s|@FAMILY@|$family|" \
		-e "s|@CPU@|$cpu|" "$HERE/cross/cross.ini.in" > "$O/cross.ini"
	if [ ! -f "$O/build/build.ninja" ]; then
		meson setup "$O/build" "$TREE" --cross-file "$O/cross.ini" \
			--buildtype=release --prefix=/usr --libdir=lib \
			-Dplatforms=fbdev -Degl-native-platform=fbdev \
			-Dgallium-drivers=sgx545 -Dvulkan-drivers= \
			-Dglx=disabled -Dopengl=false -Dgles1=disabled -Dgles2=enabled -Degl=enabled \
			-Dgbm=disabled -Dllvm=disabled -Dxmlconfig=disabled -Dexpat=disabled \
			-Dzstd=disabled -Dzlib=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
			-Dlibunwind=disabled -Dlmsensors=disabled \
			-Dallow-fallback-for=libdrm -Dlibdrm:tests=false
	fi
	ninja -C "$O/build"
	rm -rf "$O/install"
	DESTDIR="$O/install" meson install -C "$O/build" --no-rebuild --quiet
	cp "$O/build/src/gallium/winsys/sgx545/drm/sgx545_frame_test" "$O/install/usr/lib/"
	echo "build.sh: $arch -> $O/install/usr/lib"
done
