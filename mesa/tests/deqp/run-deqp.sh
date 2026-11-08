#!/bin/sh
# Run dEQP-GLES2 under deqp-runner on an EA, on our driver or the DDK, the way
# Mesa's CI runs it: pbuffers on the surfaceless EGL platform, one config, and
# the expectation files in src/gallium/drivers/sgx/ci/ (Mesa's
# *-fails/flakes/skips.txt conventions; the DDK's from mesa/tests/deqp/ddk/).  Run it ON the box, from the directory
# build-deqp.sh's out/ was copied to:
#
#   run-deqp.sh sgx|ddk [deqp-runner args...]     e.g. --fraction 20 for a quick pass
#
# Ours: our x86_64 Mesa in $MESA_LIB (default /tmp/mesa/lib).  The DDK: the
# i386 deqp-gles2 (deqp-gles2-i386) through the DDK's i386 root ($DDK_ROOT,
# default /opt/sgx-i386) and the bus-id shim, as sgx_varyab.sh runs its
# probe.  Results go to results-<who>/ (results.csv, failures.csv, logs of the
# failing groups).  One job: a second render context on this GPU has locked
# it up (sgx_screen.h), so the GPU runs one test process at a time; the
# whole run holds /tmp/gpu.lock.
set -e
WHO=${1:?sgx or ddk}; shift
D=$(cd "$(dirname "$0")" && pwd)
CI=$D/ci
OUT=$D/results-$WHO
MESA_LIB=${MESA_LIB:-/tmp/mesa/lib}
DDK_ROOT=${DDK_ROOT:-/opt/sgx-i386}

case $WHO in
sgx)
	DEQP=$D/deqp-gles2
	export LD_LIBRARY_PATH=$MESA_LIB
	export EGL_PLATFORM=surfaceless
	# a hang is a fail, not a wedged run
	export SGX_DEBUG=hangabort
	BASELINE=$CI/sgx-fails.txt FLAKES=$CI/sgx-flakes.txt SKIPS=$CI/sgx-skips.txt
	;;
ddk)
	# the i386 binary through the DDK's loader and libraries
	DEQP=$D/deqp-gles2-ddk
	cat > "$DEQP" <<EOF
#!/bin/sh
cd "$D"
LD_PRELOAD=$DDK_ROOT/usr/lib/sgx-busidshim.so exec $DDK_ROOT/lib/ld-linux.so.2 \\
	--library-path $DDK_ROOT/usr/lib:$DDK_ROOT/lib "$D/deqp-gles2-i386" "\$@"
EOF
	chmod +x "$DEQP"
	BASELINE=$CI/ddk-fails.txt FLAKES=$CI/ddk-flakes.txt SKIPS=$CI/ddk-skips.txt
	;;
*)
	echo "run-deqp.sh: sgx or ddk" >&2; exit 2 ;;
esac

for f in "$BASELINE" "$FLAKES" "$SKIPS"; do [ -f "$f" ] || : > "$f"; done
rm -rf "$OUT"
exec flock /tmp/gpu.lock "$D/deqp-runner" run \
	--deqp "$DEQP" \
	--caselist "$D/mustpass/gles2-main.txt" \
	--output "$OUT" \
	--baseline "$BASELINE" --flakes "$FLAKES" --skips "$SKIPS" \
	--jobs 1 --timeout 60 \
	"$@" \
	-- \
	--deqp-surface-type=pbuffer --deqp-surface-width=256 --deqp-surface-height=256 \
	--deqp-gl-config-name=rgba8888d24s8ms0 --deqp-visibility=hidden
