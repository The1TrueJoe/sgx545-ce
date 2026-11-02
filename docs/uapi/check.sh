#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Compiles the draft uAPI header for i386 and x86_64 and checks its layout, then
# checks the command streams: the generated kernel tables are current, the
# decoder compiles for both targets, and the stream test passes on this host.
# Needs zig (zig cc ships the Linux uAPI headers, drm/drm.h among them), a host
# cc and python3. With MESA_SRC pointing at a Mesa tree, it also runs Mesa's own
# csbgen on the stream definition.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

lib=$(zig env | sed -n 's/.*\.lib_dir = "\(.*\)",/\1/p')
drm="$lib/libc/include/any-linux-any/drm"
[ -f "$drm/drm.h" ] || { echo "no drm/drm.h under $drm" >&2; exit 1; }

warn="-Wall -Wextra -Wpedantic -Werror -Wpadded"

for target in x86-linux-gnu x86_64-linux-gnu; do
	# shellcheck disable=SC2086
	zig cc -target "$target" -std=c11 $warn -isystem "$drm" -I "$here" \
		-c "$here/check_layout.c" -o "$out/$target.o"
	echo "ok   C   $target: layout asserts pass, no implicit padding"

	# The header alone, as a C++ translation unit: Mesa has C++ users.
	printf '#include "powervr_sgx_drm.h"\n' > "$out/cxx.cpp"
	# shellcheck disable=SC2086
	zig c++ -target "$target" -std=c++17 $warn -isystem "$drm" -I "$here" \
		-c "$out/cxx.cpp" -o "$out/$target-cxx.o"
	echo "ok   C++ $target: header compiles"

	# The negative control: on i386 the size assert rejects a __u64 after a lone
	# __u32; on x86_64 that struct has a real hole, so -Wpadded rejects it. Both
	# must fail on the trap struct itself, not on anything else.
	# shellcheck disable=SC2086
	if zig cc -target "$target" -std=c11 $warn -DCHECK_NEGATIVE -isystem "$drm" -I "$here" \
		-c "$here/check_layout.c" -o "$out/neg.o" 2> "$out/neg.log"; then
		echo "FAIL $target: the negative control compiled" >&2
		exit 1
	fi
	grep -q 'check_i386_trap\|i386 packs' "$out/neg.log" || {
		cat "$out/neg.log" >&2
		echo "FAIL $target: the negative control failed for another reason" >&2
		exit 1
	}
	echo "ok   neg $target: negative control is rejected"
done

python3 "$here/gen_table.py" --check "$here/../UAPI.md"
echo "ok   docs: the layout table in UAPI.md matches layout.inc"

# Streams: one XML, generated tables on both sides.
xml="$here/powervr_sgx_kmd_stream.xml"
st="$here/stream"
gen="python3 $here/gen_stream.py"
$gen kernel-h "$xml" powervr_sgx > "$out/powervr_sgx_stream_defs.h"
$gen kernel-c "$xml" powervr_sgx > "$out/powervr_sgx_stream_defs.c"
for f in powervr_sgx_stream_defs.h powervr_sgx_stream_defs.c; do
	cmp -s "$out/$f" "$st/$f" || {
		echo "FAIL stream/$f is stale: rerun gen_stream.py kernel-${f##*.} (see its header)" >&2
		exit 1
	}
done
echo "ok   stream: kernel tables are current with $(basename "$xml")"

$gen kernel-h "$st/test/ext_fixture.xml" fixture > "$out/fixture_stream_defs.h"
$gen kernel-c "$st/test/ext_fixture.xml" fixture > "$out/fixture_stream_defs.c"
$gen user "$xml" > "$out/sgx_kmd_stream_pack.h"

kwarn="-Wall -Wextra -Wpedantic -Werror"
for target in x86-linux-gnu x86_64-linux-gnu; do
	for c in powervr_sgx_stream.c powervr_sgx_cmd_checks.c powervr_sgx_stream_defs.c; do
		# shellcheck disable=SC2086
		zig cc -target "$target" -std=gnu11 $kwarn -I "$st/test/shim" -I "$st" \
			-c "$st/$c" -o "$out/$target-${c%.c}.o"
	done
	echo "ok   stream: decoder compiles for $target"
done

# shellcheck disable=SC2086
cc -std=gnu11 $kwarn -I "$st/test/shim" -I "$st" -I "$out" -o "$out/stream_test" \
	"$st/test/stream_test.c" "$st/powervr_sgx_stream.c" "$st/powervr_sgx_cmd_checks.c" \
	"$st/powervr_sgx_stream_defs.c" "$out/fixture_stream_defs.c"
"$out/stream_test"
echo "ok   stream: decoder test passes on this host"

if [ -n "${MESA_SRC:-}" ]; then
	python3 "$MESA_SRC/src/imagination/csbgen/gen_pack_header.py" "$xml" > "$out/mesa_kmd_stream.h"
	# The address hooks a csbgen user defines, as pvr_csb.h does.
	cat > "$out/mesa.c" <<-EOF
		#include <stdint.h>
		typedef struct { uint64_t addr; } dev_addr_t;
		#define __pvr_address_type dev_addr_t
		#define __pvr_get_address(a) (a).addr
		#define __pvr_make_address(v) ((dev_addr_t){ .addr = (v) })
		#include "mesa_kmd_stream.h"
		int main(void) { return SGX_KMD_STREAM_HDR_length != 2; }
	EOF
	cc -std=c11 -Wall -Werror -Wno-unused-function -I "$MESA_SRC/src/imagination" -I "$out" -c "$out/mesa.c" -o "$out/mesa.o"
	echo "ok   stream: Mesa's csbgen reads the XML and its output compiles"
else
	echo "skip stream: MESA_SRC not set, Mesa's csbgen not run"
fi

python3 "$here/gen_stream.py" --check "$xml" "$here/../UAPI.md"
echo "ok   docs: the stream table in UAPI.md matches $(basename "$xml")"
