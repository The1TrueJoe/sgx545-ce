#!/bin/sh
# Build dEQP-GLES2 and deqp-runner for the EA (x86_64, glibc 2.36 ABI), the
# way Mesa's CI builds them, plus mesa/tests/deqp/patches (.gitlab-ci/container/build-deqp.sh and
# build-deqp-runner.sh of the Mesa release in mesa/VERSION): the same CTS
# release, backports and patches; dEQP's surfaceless target, which renders to
# pbuffers through EGL_PLATFORM_SURFACELESS_MESA (mesa/patches/0003 lets it
# find pvrsrvkm's primary node).
#
#   mesa/tests/deqp/build-deqp.sh [x86_64|i386]    (after mesa/build.sh for that arch)
#
# x86_64 (the default) is ours; i386 is for the DDK (the DDK is an i386
# client): it links against our i386 Mesa only for the sonames, and
# run-deqp.sh runs it on the DDK's libraries.
#
# Sources and build live in $DEQP_WORK (default ~/.cache/sgx545/deqp: a few
# GB, kept out of the repo and out of /tmp); the result is $DEQP_WORK/out:
#   deqp-gles2 (deqp-gles2-i386), its data, mustpass/gles2-main.txt, deqp-runner, version
# which run-deqp.sh copies to the box.
#
# Needs: git, python3, cmake, ninja, zig, rustup with a stable toolchain
# (cargo-zigbuild is installed into $DEQP_WORK/cargo).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
. "$REPO/mesa/VERSION"
W=${DEQP_WORK:-$HOME/.cache/sgx545/deqp}
B=${SGX_MESA_BUILD:-$REPO/build/mesa}
ARCH=${1:-x86_64}
MESA=$B/$ARCH/install/usr
CI=$B/mesa-$MESA_VERSION/.gitlab-ci
case $ARCH in
x86_64) TARGET=x86_64-linux-gnu.2.36; CPU=x86_64; SUFFIX= ;;
i386) TARGET=x86-linux-gnu.2.17; CPU=i686; SUFFIX=-i386 ;;    # the DDK's root, as the probe
*) echo "build-deqp.sh: x86_64 or i386" >&2; exit 2 ;;
esac

# what Mesa's CI uses (build-deqp.sh, build-deqp-runner.sh)
DEQP_GLES_VERSION=$(sed -n 's/^DEQP_GLES_VERSION=//p' "$CI/container/build-deqp.sh")
DEQP_RUNNER_VERSION=$(sed -n 's/^DEQP_RUNNER_VERSION=//p' "$CI/container/build-deqp-runner.sh")
GLES_BACKPORTS=$(sed -n '/^gles_cts_commits_to_backport=(/,/^)/p' "$CI/container/build-deqp.sh" | grep -E '^ +[0-9a-f]{40}$')
GLES_PATCHES=$(sed -n '/^gles_cts_patch_files=(/,/^)/p' "$CI/container/build-deqp.sh" | grep -E '\.patch$')
[ -n "$DEQP_GLES_VERSION" ] && [ -n "$DEQP_RUNNER_VERSION" ] || {
	echo "build-deqp.sh: no Mesa CI scripts in $CI (run mesa/build.sh first)" >&2; exit 1; }
[ -f "$MESA/lib/libEGL.so.1" ] || { echo "build-deqp.sh: no $ARCH Mesa in $MESA" >&2; exit 1; }
mkdir -p "$W"

# ---- the CTS, as Mesa's CI patches it
if [ ! -f "$W/src/.sgx-patched" ]; then
	rm -rf "$W/src"
	git clone --depth 1 --branch "opengl-es-cts-$DEQP_GLES_VERSION" \
		https://github.com/KhronosGroup/VK-GL-CTS.git "$W/src"
	(
		cd "$W/src"
		export GIT_COMMITTER_NAME="powervr_sgx" GIT_COMMITTER_EMAIL="powervr_sgx@localhost"
		export GIT_AUTHOR_NAME="$GIT_COMMITTER_NAME" GIT_AUTHOR_EMAIL="$GIT_COMMITTER_EMAIL"
		for c in $GLES_BACKPORTS; do
			curl -fsL "https://github.com/KhronosGroup/VK-GL-CTS/commit/$c.patch" | git am -q
		done
		for p in $GLES_PATCHES; do
			git am -q < "$CI/container/patches/$p"
		done
		# ours: the surfaceless platform on an EGL 1.4 without
		# EGL_KHR_create_context or client extensions (the DDK, for the A/B)
		for p in "$HERE"/patches/*.patch; do
			git apply "$p"
		done
		python3 external/fetch_sources.py
	)
	touch "$W/src/.sgx-patched"
fi

# ---- deqp-gles2 for the EA
# zig as the cross compiler, its target baked into wrappers (CMake drops
# compiler arguments given any other way)
printf '#!/bin/sh\nexec zig cc -target %s "$@"\n' "$TARGET" > "$W/zig-cc-$ARCH"
printf '#!/bin/sh\nexec zig c++ -target %s "$@"\n' "$TARGET" > "$W/zig-c++-$ARCH"
printf '#!/bin/sh\nexec zig ar "$@"\n' > "$W/zig-ar"
printf '#!/bin/sh\nexec zig ranlib "$@"\n' > "$W/zig-ranlib"
chmod +x "$W/zig-cc-$ARCH" "$W/zig-c++-$ARCH" "$W/zig-ar" "$W/zig-ranlib"
cat > "$W/zig-toolchain-$ARCH.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $CPU)
set(CMAKE_C_COMPILER "$W/zig-cc-$ARCH")
set(CMAKE_CXX_COMPILER "$W/zig-c++-$ARCH")
set(CMAKE_AR "$W/zig-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "$W/zig-ranlib" CACHE FILEPATH "")
set(CMAKE_EXECUTABLE_FORMAT ELF)
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
set(CMAKE_FIND_ROOT_PATH "$MESA")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
PKG_CONFIG_LIBDIR="$MESA/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="" \
cmake -S "$W/src" -B "$W/build-$ARCH" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$W/zig-toolchain-$ARCH.cmake" \
	-DDEQP_TARGET=surfaceless \
	-DCMAKE_BUILD_TYPE=Release \
	-DSELECTED_BUILD_TARGETS=deqp-gles2 \
	-DCMAKE_EXE_LINKER_FLAGS="-L$MESA/lib" \
	-DGLES2_LIBRARIES="$MESA/lib/libGLESv2.so" -DGLES3_LIBRARIES="$MESA/lib/libGLESv2.so" \
	-DGLES2_INCLUDE_PATH="$MESA/include" -DGLES3_INCLUDE_PATH="$MESA/include"
ninja -C "$W/build-$ARCH" deqp-gles2
mkdir -p "$W/out/mustpass"
cp "$W/build-$ARCH/modules/gles2/deqp-gles2" "$W/out/deqp-gles2$SUFFIX"
[ -d "$W/out/gles2" ] || cp -R "$W/src/data/gles2" "$W/out/"
cp "$W/src/external/openglcts/data/gl_cts/data/mustpass/gles/aosp_mustpass/main/gles2-main.txt" "$W/out/mustpass/"
[ "$ARCH" = x86_64 ] || { echo "build-deqp.sh: $W/out/deqp-gles2$SUFFIX"; exit 0; }

# ---- deqp-runner
if [ ! -x "$W/cargo/bin/cargo-zigbuild" ]; then
	cargo install --locked --root "$W/cargo" cargo-zigbuild
fi
# rustup's own toolchain, called directly (Homebrew's cargo/rustc on PATH has
# no std for the target): RUST_TOOLCHAIN, default rustup's stable
RT=${RUST_TOOLCHAIN:-$(ls -d "$HOME"/.rustup/toolchains/stable-* | head -1)}
RUSTUP=$(command -v rustup || echo /opt/homebrew/opt/rustup/bin/rustup)
"$RUSTUP" target add --toolchain stable x86_64-unknown-linux-gnu >/dev/null
if [ ! -d "$W/deqp-runner" ]; then
	git clone --depth 1 --branch "v$DEQP_RUNNER_VERSION" \
		https://gitlab.freedesktop.org/mesa/deqp-runner.git "$W/deqp-runner"
fi
(cd "$W/deqp-runner" && PATH="$W/cargo/bin:$RT/bin:$PATH" DYLD_FALLBACK_LIBRARY_PATH="$RT/lib" \
	RUSTC="$RT/bin/rustc" "$RT/bin/cargo" zigbuild --release --locked \
	--target "x86_64-unknown-linux-gnu.2.36" --bin deqp-runner)

# ---- the rest of what goes to the box
cp "$HERE/run-deqp.sh" "$W/out/"
rm -rf "$W/out/ci" && cp -R "$REPO/mesa/src/gallium/drivers/sgx/ci" "$W/out/ci" &&
	cp "$HERE"/ddk/ddk-*.txt "$W/out/ci/"
cp "$W/deqp-runner/target/x86_64-unknown-linux-gnu/release/deqp-runner" "$W/out/"
{
	echo "dEQP GLES opengl-es-cts-$DEQP_GLES_VERSION (Mesa $MESA_VERSION CI's backports and patches)"
	echo "deqp-runner $DEQP_RUNNER_VERSION"
} > "$W/out/version"
echo "build-deqp.sh: $W/out"
