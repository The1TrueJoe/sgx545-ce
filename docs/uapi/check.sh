#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Compiles the draft uAPI header for i386 and x86_64 and checks its layout, then
# checks the client commands: the generated header block and kernel tables are
# current, the checker compiles for both targets, and its test passes on this host.
# Needs zig (zig cc ships the Linux uAPI headers, drm/drm.h among them), a host
# cc and python3.

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

# Commands: one XML, the header block and the kernel tables generated from it.
xml="$here/powervr_sgx_cmd.xml"
cmd="$here/cmd"
gen="python3 $here/gen_cmd.py"
$gen kernel-h "$xml" > "$out/pvrsgx_cmd_defs.h"
$gen kernel-c "$xml" > "$out/pvrsgx_cmd_defs.c"
for f in pvrsgx_cmd_defs.h pvrsgx_cmd_defs.c; do
	cmp -s "$out/$f" "$cmd/$f" || {
		echo "FAIL cmd/$f is stale: rerun gen_cmd.py kernel-${f##*.} (see its header)" >&2
		exit 1
	}
done
$gen --check "$xml" "$here/powervr_sgx_drm.h" "$here/../UAPI.md"
echo "ok   cmd: header block, kernel tables and UAPI.md table are current with $(basename "$xml")"

kwarn="-Wall -Wextra -Wpedantic -Werror"
for target in x86-linux-gnu x86_64-linux-gnu; do
	for c in pvrsgx_cmd.c pvrsgx_cmd_checks.c pvrsgx_cmd_defs.c; do
		# shellcheck disable=SC2086
		zig cc -target "$target" -std=gnu11 $kwarn -I "$cmd/test/shim" -I "$cmd" \
			-c "$cmd/$c" -o "$out/$target-${c%.c}.o"
	done
	echo "ok   cmd: checker compiles for $target"
done

# shellcheck disable=SC2086
cc -std=gnu11 $kwarn -I "$cmd/test/shim" -I "$cmd" -o "$out/cmd_test" \
	"$cmd/test/cmd_test.c" "$cmd/pvrsgx_cmd.c" "$cmd/pvrsgx_cmd_checks.c" "$cmd/pvrsgx_cmd_defs.c"
"$out/cmd_test"
echo "ok   cmd: checker test passes on this host"
