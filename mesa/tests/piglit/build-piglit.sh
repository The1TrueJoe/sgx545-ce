#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# Build piglit and its runner for the EA (x86_64, glibc 2.36 ABI), for the
# desktop GL work (AGENTS.md M6), the way Mesa's CI builds piglit
# (.gitlab-ci/container/build-piglit.sh of the Mesa release in mesa/VERSION:
# the same revision and patch).  Desktop GL on a box with no X goes through
# libglvnd: piglit and waffle link libOpenGL.so.0 and libEGL.so.1 from glvnd,
# which dispatch to our libEGL_mesa (a second Mesa build with -Dopengl=true
# -Dglvnd=enabled, in $SGX_MESA_BUILD/x86_64-gl; the shipping GLES build
# is left alone).
#
#   mesa/tests/piglit/build-piglit.sh
#
# Sources and builds live in $PIGLIT_WORK (default ~/.cache/sgx545/piglit);
# the result is $PIGLIT_WORK/out, which run-piglit.sh copies to the box:
#   lib/ (glvnd, waffle, our Mesa, 50_mesa.json), piglit/ (bin, tests, the
#   gpu profile), piglit-runner
#
# Needs: git, cmake, meson, ninja, zig, python3 with mako and numpy, rustup
# with a stable toolchain (cargo-zigbuild, as build-deqp.sh installs it).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
. "$REPO/mesa/VERSION"
W=${PIGLIT_WORK:-$HOME/.cache/sgx545/piglit}
B=${SGX_MESA_BUILD:-$REPO/build/mesa}
CI=$B/mesa-$MESA_VERSION/.gitlab-ci
TARGET=x86_64-linux-gnu.2.36
SYS=$W/sysroot                 # what the later builds compile and link against
GLVND_VERSION=v1.7.0
WAFFLE_VERSION=v1.8.1
# master's C11 threads fixes: 1.8.1's clash with glibc 2.34+'s once_flag
WAFFLE_PICKS="6b3c46483844faba4dfe7dd90dc2b0cd06e67158 faa301a9f084a03d26aab89f2f6022b074eeeb96"
PIGLIT_REV=$(sed -n 's/^REV="\(.*\)"$/\1/p' "$CI/container/build-piglit.sh")
[ -n "$PIGLIT_REV" ] || { echo "build-piglit.sh: no Mesa CI scripts in $CI (run mesa/build.sh first)" >&2; exit 1; }
mkdir -p "$W" "$SYS"

# zig as the cross compiler, for meson and CMake
sed -e "s|@ZIGCC@|$REPO/mesa/cross/zigcc|" -e "s|@TARGET@|$TARGET|" -e "s|@FAMILY@|x86_64|" \
	-e "s|@CPU@|x86_64|" "$REPO/mesa/cross/cross.ini.in" |
	sed "s|^pkg_config_libdir = .*|pkg_config_libdir = ['$SYS/usr/lib/pkgconfig', '$SYS/usr/share/pkgconfig']\nsys_root = '$SYS'|" > "$W/cross.ini"
printf '#!/bin/sh\nexec zig cc -target %s -g0 "$@"\n' "$TARGET" > "$W/zig-cc"
printf '#!/bin/sh\nexec zig c++ -target %s -g0 "$@"\n' "$TARGET" > "$W/zig-c++"
printf '#!/bin/sh\nexec zig ar "$@"\n' > "$W/zig-ar"
printf '#!/bin/sh\nexec zig ranlib "$@"\n' > "$W/zig-ranlib"
chmod +x "$W/zig-cc" "$W/zig-c++" "$W/zig-ar" "$W/zig-ranlib"
cat > "$W/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $W/zig-cc)
set(CMAKE_CXX_COMPILER $W/zig-c++)
set(CMAKE_AR $W/zig-ar)
set(CMAKE_RANLIB $W/zig-ranlib)
set(CMAKE_FIND_ROOT_PATH $SYS/usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
# (-I/usr/include and -L/usr/lib are kept, so the sysroot gets prefixed to them)
export PKG_CONFIG_LIBDIR="$SYS/usr/lib/pkgconfig:$SYS/usr/share/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$SYS" \
	PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1 PKG_CONFIG_ALLOW_SYSTEM_LIBS=1

# ---- libglvnd: libOpenGL, libEGL (the dispatcher), libGLdispatch, GL headers
[ -d "$W/glvnd" ] || git clone -q --depth 1 --branch "$GLVND_VERSION" \
	https://gitlab.freedesktop.org/glvnd/libglvnd.git "$W/glvnd"
if [ ! -f "$W/glvnd/build/build.ninja" ]; then
	meson setup "$W/glvnd/build" "$W/glvnd" --cross-file "$W/cross.ini" --buildtype=release \
		--prefix=/usr --libdir=lib -Dx11=disabled -Dglx=disabled -Dtls=true -Dasm=disabled
fi
ninja -C "$W/glvnd/build"
DESTDIR="$SYS" meson install -C "$W/glvnd/build" --no-rebuild --quiet
echo "build-piglit.sh: glvnd -> $SYS"

# ---- waffle: piglit's window-system layer, on EGL surfaceless (and wflinfo, a quick check)
if [ ! -d "$W/waffle" ]; then
	git clone -q https://gitlab.freedesktop.org/mesa/waffle.git "$W/waffle"
	git -C "$W/waffle" checkout -q "$WAFFLE_VERSION"
	git -C "$W/waffle" -c user.name=sgx545-ce -c user.email=sgx545-ce@localhost cherry-pick -x $WAFFLE_PICKS
	# a disabled wayland's dependency('egl') replaces the one surfaceless found;
	# surfaceless includes gbm.h and uses nothing from it (no GBM in our Mesa)
	sed -i.orig "s|^  dep_egl = dependency('egl', required : get_option('wayland'))$|  if not dep_egl.found()\\n    dep_egl = dependency('egl', required : get_option('wayland'))\\n  endif|" \
		"$W/waffle/meson.build"
	sed -i.orig '/^#include <gbm.h>$/d' "$W/waffle/src/waffle/surfaceless_egl/sl_platform.h"
fi
if [ ! -f "$W/waffle/build/build.ninja" ]; then
	meson setup "$W/waffle/build" "$W/waffle" --cross-file "$W/cross.ini" --buildtype=release \
		--prefix=/usr --libdir=lib -Dglx=disabled -Dwayland=disabled -Dx11_egl=disabled -Dgbm=disabled \
		-Dsurfaceless_egl=enabled -Dbuild-examples=false
fi
ninja -C "$W/waffle/build"
DESTDIR="$SYS" meson install -C "$W/waffle/build" --no-rebuild --quiet
echo "build-piglit.sh: waffle -> $SYS"

# ---- piglit, at Mesa CI's revision with its patch, built in its tree as CI does
# (piglit-runner wants bin/ and tests/, the generated profiles, in one folder)
if [ ! -d "$W/piglit" ]; then
	git clone -q --single-branch --no-checkout https://gitlab.freedesktop.org/mesa/piglit.git "$W/piglit"
	git -C "$W/piglit" checkout -q "$PIGLIT_REV"
	patch -d "$W/piglit" -p1 < "$CI/piglit/disable-vs_in.diff"
	# the EGL tests are on for PIGLIT_HAS_EGL (the surfaceless platform needs it);
	# tests/egl's own programs need X11
	sed -i.orig 's|^	add_subdirectory (egl)$|	if (PIGLIT_USE_X11)\n		add_subdirectory (egl)\n	endif ()|' \
		"$W/piglit/tests/CMakeLists.txt"
fi
# CMake's FindOpenGL wants a libGL; glvnd without GLX has libOpenGL, whose
# soname the tests then record (waffle opens libGL.so.1: run-piglit.sh links it)
ln -sf libOpenGL.so.0 "$SYS/usr/lib/libGL.so"
if [ ! -f "$W/piglit/build.ninja" ]; then
	cmake -S "$W/piglit" -B "$W/piglit" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_TOOLCHAIN_FILE="$W/toolchain.cmake" -DPYTHON_EXECUTABLE="$(command -v python3)" \
		-DOpenGL_GL_PREFERENCE=LEGACY -DPIGLIT_USE_WAFFLE=ON -DPIGLIT_USE_X11=OFF -DPIGLIT_USE_GBM=OFF \
		-DPIGLIT_USE_WAYLAND=OFF -DPIGLIT_BUILD_GLX_TESTS=OFF -DPIGLIT_BUILD_EGL_TESTS=ON \
		-DPIGLIT_BUILD_GL_TESTS=ON -DPIGLIT_BUILD_GLES1_TESTS=OFF -DPIGLIT_BUILD_GLES2_TESTS=OFF \
		-DPIGLIT_BUILD_GLES3_TESTS=OFF -DPIGLIT_BUILD_VK_TESTS=OFF -DPIGLIT_BUILD_DMA_BUF_TESTS=OFF
fi
ninja -C "$W/piglit"
echo "build-piglit.sh: piglit -> $W/piglit"

# ---- piglit-runner: deqp-runner's sibling, from the checkout build-deqp.sh made
DW=${DEQP_WORK:-$HOME/.cache/sgx545/deqp}
[ -d "$DW/deqp-runner" ] && [ -x "$DW/cargo/bin/cargo-zigbuild" ] || {
	echo "build-piglit.sh: run mesa/tests/deqp/build-deqp.sh first (deqp-runner and cargo-zigbuild)" >&2; exit 1; }
RT=${RUST_TOOLCHAIN:-$(ls -d "$HOME"/.rustup/toolchains/stable-* | head -1)}
(cd "$DW/deqp-runner" && PATH="$DW/cargo/bin:$RT/bin:$PATH" DYLD_FALLBACK_LIBRARY_PATH="$RT/lib" \
	RUSTC="$RT/bin/rustc" "$RT/bin/cargo" zigbuild --release --locked \
	--target "x86_64-unknown-linux-gnu.2.36" --bin piglit-runner)

# ---- what goes to the box
rm -rf "$W/out" && mkdir -p "$W/out/lib"
cp -a "$SYS"/usr/lib/*.so* "$B/x86_64-gl/install/usr/lib"/*.so* "$W/out/lib/"
ln -sf libOpenGL.so.0 "$W/out/lib/libGL.so.1"         # what waffle dlopens for desktop GL
# piglit's tree without sources and build files, as Mesa's CI ships it
(cd "$W/piglit" && find . \( -name .git -o -name CMakeFiles -o -name '*.[chao]' -o -name '*.cpp' \
	-o -name '*ninja*' -o -name '*cmake*' -o -name target_api \) -prune -o -type f -print |
	tar cf - -T -) | (mkdir -p "$W/out/piglit" && cd "$W/out/piglit" && tar xf -)
cp "$DW/deqp-runner/target/x86_64-unknown-linux-gnu/release/piglit-runner" "$W/out/"
cp "$HERE/run-piglit.sh" "$W/out/" 2>/dev/null || true
echo "piglit $PIGLIT_REV (Mesa $MESA_VERSION CI's revision and patch), libglvnd $GLVND_VERSION, waffle $WAFFLE_VERSION" > "$W/out/version"
echo "build-piglit.sh: $W/out"
