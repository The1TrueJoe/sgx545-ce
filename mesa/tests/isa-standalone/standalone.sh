#!/bin/bash
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# Build the ISA library (mesa/src/sgx/isa), sgx_isa_test, sgx-asm and
# sgx-disasm without a Mesa build, against an unpacked Mesa tree of
# mesa/VERSION's release (needs python3 with mako):
#
#    mesa/tests/isa-standalone/standalone.sh <mesa-tree> [out-dir]  &&
#       <out-dir>/sgx_isa_test mesa/src/sgx/isa/tests/*.txt
set -e
T=$(cd "$(dirname "$0")" && pwd)
HERE=$(cd "$T/../../src/sgx/isa" && pwd)
M=$(cd "$1" && pwd)
B=${2:-$T/build}
PY=${PYTHON:-python3}
mkdir -p "$B/obj" "$B/isaspec"
cp "$M"/src/compiler/isaspec/*.py "$B/isaspec/"
(cd "$B" && mkdir -p a/src/compiler && rm -rf a/src/compiler/isaspec &&
 cp -R isaspec a/src/compiler/ &&
 P=$(ls "$T"/../../patches/*-isaspec-derived-bitset-params-and-override-fields.patch) &&
 # (a tree that already carries the patch is used as it is)
 { ! patch -d a -p1 --forward --batch --dry-run -s < "$P" >/dev/null 2>&1 ||
   patch -d a -p1 --forward --batch -s < "$P"; } &&
 cp a/src/compiler/isaspec/*.py isaspec/)
W=$B/isaspec
$PY $W/decode.py --xml $HERE/sgx.xml --out-c $B/sgx-isa.c --out-h $B/sgx-isa.h
$PY $W/encode.py --xml $HERE/sgx.xml --out-h $B/sgx-isa-encode.h
PYTHONPATH=$W $PY $HERE/sgx_isa_enums.py --xml $HERE/sgx.xml --out-h $B/sgx-isa-enums.h
CF=(-std=gnu11 -g -O1 -I $B -I $HERE -I $M/src -I $M/include -I $M/src/compiler/isaspec
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -DHAVE_TIMESPEC_GET -DHAVE_STRNDUP)
# as meson does: glibc's byte order comes through <endian.h> (util/u_endian.h)
if echo '#include <endian.h>' | ${CC:-cc} -E -x c - >/dev/null 2>&1; then
  CF+=(-DHAVE_ENDIAN_H)
fi
OBJS=()
for f in $B/sgx-isa.c $HERE/sgx_isa_encode.c $HERE/sgx_isa_disasm.c \
         $HERE/sgx_isa_asm.c $HERE/sgx_isa_asm_int.c $HERE/sgx_isa_coissue.c $T/standalone_stubs.c \
         $M/src/compiler/isaspec/isaspec.c $M/src/util/ralloc.c $M/src/util/hash_table.c \
         $M/src/util/half_float.c $M/src/util/softfloat.c; do
  o=$B/obj/$(basename $f .c).o
  ${CC:-cc} "${CF[@]}" -c $f -o $o
  OBJS+=($o)
done
${CC:-cc} "${CF[@]}" $HERE/tests/sgx_isa_test.c "${OBJS[@]}" -o $B/sgx_isa_test
${CC:-cc} "${CF[@]}" $HERE/assembler.c "${OBJS[@]}" -o $B/sgx-asm
${CC:-cc} "${CF[@]}" $HERE/disasm.c "${OBJS[@]}" -o $B/sgx-disasm
echo "$B/sgx_isa_test"
