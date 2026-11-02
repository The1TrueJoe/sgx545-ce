#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Compiles the draft uAPI header for i386 and x86_64 and checks its layout.
# Needs zig (zig cc ships the Linux uAPI headers, drm/drm.h among them) and python3.
# Compile only: nothing is linked or run on the target.

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
	printf '#include "sgx_drm_draft.h"\n' > "$out/cxx.cpp"
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
