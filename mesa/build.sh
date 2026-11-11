#!/bin/sh
# Build Mesa with the sgx gallium driver (over powervr_sgx, or pvrsrvkm
# through libpvr), the EGL fbdev platform, and GBM.
#
#   mesa/build.sh [x86_64] [i386] [x86_64-gl] [x86_64-x11] [debian]   (default: x86_64 i386)
#
# Fetches the release in mesa/VERSION (checked against its sha256), unpacks
# it under $SGX_MESA_BUILD (default build/mesa in the repo), applies
# mesa/patches (the series for upstream, then mesa/patches/pvrsrvkm: the DDK
# services driver's pieces, not for upstream), copies the mesa/src overlay over
# the tree, and builds with
# zig cc for each target:
#   x86_64  glibc 2.36 ABI: newer glibc userspaces (2.42) run it;
#   i386    glibc 2.36: the Debian bookworm i386 root the Minecraft runtime uses.
#   x86_64-gl  x86_64 with desktop GL through libglvnd (libEGL_mesa), for
#           piglit (mesa/tests/piglit); finds glvnd in $GLVND_SYSROOT.
#   x86_64-x11 x86_64 with desktop GL, GLX and the X11 platform through
#           libglvnd, for an X server (DRI3 + Present) on the EA; compiles
#           against the Debian root in $X11_SYSROOT (mesa/tests/x11/mkroot.py).
#   debian  a native build on Debian (mesa/debian: CI) for /opt/sgx, for users.
# Installs into $SGX_MESA_BUILD/<arch>/install (lib/libEGL.so.1,
# lib/libGLESv2.so.2, lib/libgallium-<ver>.so, ...), and copies the winsys
# tests (sgx_frame_test, sgx_hwt) next to it.
#
# Needs: zig (0.14+), meson, ninja, python3 with mako and pyyaml, bison 3,
# flex, network for the tarball and the libdrm wrap.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
. "$HERE/VERSION"
B=${SGX_MESA_BUILD:-$REPO/build/mesa}
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

# the tree is unpacked and patched again whenever mesa/patches changes
PATCHSUM=$(cat "$HERE"/patches/*.patch "$HERE"/patches/pvrsrvkm/*.patch | shasum -a 256 | cut -d' ' -f1)
if [ "$(cat "$TREE/.sgx-patched" 2>/dev/null)" != "$PATCHSUM" ]; then
	rm -rf "$TREE"
	tar -xJf "$TARBALL" -C "$B"
	for p in "$HERE"/patches/*.patch "$HERE"/patches/pvrsrvkm/*.patch; do
		patch -d "$TREE" -p1 --batch --forward < "$p"
	done
	echo "$PATCHSUM" > "$TREE/.sgx-patched"
fi
# the overlay, every build
(cd "$HERE/src" && tar cf - .) | (cd "$TREE/src" && tar xf -)
ln -sfn "$REPO/kernel/src" "$TREE/include/sgx-kernel"
ln -sfn "$REPO/libpvr" "$TREE/src/gallium/winsys/sgx/drm/libpvr"
# the powervr_sgx uAPI (Mesa carries uAPI headers in include/drm-uapi), and its
# command streams' XML with the generator meson runs on it (src/sgx/kmd_stream),
# from their one definition
# (SGX_UAPI_H: another revision of the header, to build against a kernel branch)
cp "${SGX_UAPI_H:-$REPO/drm/powervr_sgx/include/uapi/drm/powervr_sgx_drm.h}" "$TREE/include/drm-uapi/"
cp "$REPO/docs/uapi/powervr_sgx_kmd_stream.xml" "$REPO/docs/uapi/gen_stream.py" \
	"$TREE/src/sgx/kmd_stream/"
rm -f "$TREE/src/gallium/winsys/sgx/drm/powervr_sgx_kmd_stream.h"

for arch in $ARCHES; do
	case $arch in
	x86_64|x86_64-gl|x86_64-x11) target=x86_64-linux-gnu.2.36; family=x86_64; cpu=x86_64 ;;
	i386) target=x86-linux-gnu.2.36; family=x86; cpu=i686 ;;
	debian) ;;
	*) echo "build.sh: unknown arch $arch" >&2; exit 1 ;;
	esac
	if [ "$arch" = debian ]; then
		# Native, on a Debian system with Mesa's build dependencies, for /opt/sgx:
		# GLX and EGL through glvnd, GBM, X11 and Wayland; sgx on powervr_sgx, and
		# llvmpipe (as Debian ships it) so a machine without the GPU or its driver
		# keeps a usable GL.
		O=$B/debian
		[ -f "$O/build/build.ninja" ] ||
			meson setup "$O/build" "$TREE" --buildtype=release --prefix=/opt/sgx --libdir=lib \
				-Dplatforms=x11,wayland -Degl-native-platform=x11 -Dglx=dri -Dglvnd=enabled \
				-Dgallium-drivers=sgx,llvmpipe -Dvulkan-drivers= -Dopengl=true \
				-Dgles1=enabled -Dgles2=enabled -Degl=enabled -Dgbm=enabled -Dllvm=enabled \
				-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled
		ninja -C "$O/build" all src/gallium/winsys/sgx/drm/sgx_frame_test src/gallium/winsys/sgx/drm/sgx_hwt
		rm -rf "$O/install"
		DESTDIR="$O/install" meson install -C "$O/build" --no-rebuild --quiet
		cp "$O/build/src/gallium/winsys/sgx/drm/sgx_frame_test" "$O/build/src/gallium/winsys/sgx/drm/sgx_hwt" "$O/install/opt/sgx/lib/"
		echo "build.sh: debian -> $O/install/opt/sgx"
		continue
	fi
	O=$B/$arch
	mkdir -p "$O"
	sed -e "s|@ZIGCC@|$HERE/cross/zigcc|" -e "s|@TARGET@|$target|" -e "s|@FAMILY@|$family|" \
		-e "s|@CPU@|$cpu|" "$HERE/cross/cross.ini.in" > "$O/cross.ini"
	api="-Dopengl=false -Dglvnd=disabled"
	if [ "$arch" = x86_64-gl ]; then
		# a plain sysroot (usr/lib/pkgconfig) or a Debian one (multiarch)
		pc=
		for d in usr/lib/pkgconfig usr/lib/x86_64-linux-gnu/pkgconfig; do
			[ -f "${GLVND_SYSROOT:-/nonexistent}/$d/libglvnd.pc" ] && pc="$GLVND_SYSROOT/$d"
		done
		[ -n "$pc" ] || {
			echo "build.sh: x86_64-gl needs libglvnd in \$GLVND_SYSROOT" >&2; exit 1; }
		sed -i.bak "s|^pkg_config_libdir = .*|pkg_config_libdir = ['$pc', '$GLVND_SYSROOT/usr/share/pkgconfig']\nsys_root = '$GLVND_SYSROOT'|" "$O/cross.ini"
		api="-Dopengl=true -Dglvnd=enabled"
	fi
	platforms="-Dplatforms=fbdev -Degl-native-platform=fbdev -Dglx=disabled"
	# the DDK's services backend too, for the fbdev targets
	pvrsrvkm=-Dgallium-sgx-pvrsrvkm=true
	if [ "$arch" = x86_64-x11 ]; then
		R=${X11_SYSROOT:?x86_64-x11 needs a Debian root in X11_SYSROOT (mesa/tests/x11/mkroot.py)}
		sed -i.bak "s|^pkg_config_libdir = .*|pkg_config_libdir = ['$R/usr/lib/x86_64-linux-gnu/pkgconfig', '$R/usr/share/pkgconfig']\nsys_root = '$R'|" "$O/cross.ini"
		# the root's headers after zig's own (its libc's win), its libraries for the link
		x="'-idirafter', '$R/usr/include', '-L$R/usr/lib/x86_64-linux-gnu'"
		sed -i.bak "s|^c = \[\(.*\)\]|c = [\1, $x]|; s|^cpp = \[\(.*\)\]|cpp = [\1, $x]|" "$O/cross.ini"
		api="-Dopengl=true -Dglvnd=enabled"
		platforms="-Dplatforms=x11 -Degl-native-platform=x11 -Dglx=dri -Dxlib-lease=disabled"
		pvrsrvkm=
	fi
	if [ ! -f "$O/build/build.ninja" ]; then
		meson setup "$O/build" "$TREE" --cross-file "$O/cross.ini" \
			--buildtype=release --prefix=/usr --libdir=lib \
			$platforms \
			-Dgallium-drivers=sgx -Dvulkan-drivers= $pvrsrvkm \
			$api -Dgles1=enabled -Dgles2=enabled -Degl=enabled \
			-Dgbm=enabled -Dllvm=disabled -Dxmlconfig=disabled -Dexpat=disabled \
			-Dzstd=disabled -Dzlib=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
			-Dlibunwind=disabled -Dlmsensors=disabled \
			-Dallow-fallback-for=libdrm -Dlibdrm:tests=false
	fi
	ninja -C "$O/build" all src/gallium/winsys/sgx/drm/sgx_frame_test src/gallium/winsys/sgx/drm/sgx_hwt
	rm -rf "$O/install"
	DESTDIR="$O/install" meson install -C "$O/build" --no-rebuild --quiet
	cp "$O/build/src/gallium/winsys/sgx/drm/sgx_frame_test" "$O/build/src/gallium/winsys/sgx/drm/sgx_hwt" "$O/install/usr/lib/"
	echo "build.sh: $arch -> $O/install/usr/lib"
done
