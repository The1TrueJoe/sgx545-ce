#!/bin/bash
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# Build the ISA library and sgx545_isa_test without a Mesa build, against an
# unpacked Mesa tree of mesa/VERSION's release (needs python3 with mako):
#
#    tests/standalone.sh <mesa-tree> [out-dir]  &&  <out-dir>/sgx545_isa_test tests/*.txt
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
M=$(cd "$1" && pwd)
B=${2:-$HERE/tests/build}
PY=${PYTHON:-python3}
mkdir -p "$B/obj" "$B/isaspec"
cp "$M"/src/compiler/isaspec/*.py "$B/isaspec/"
(cd "$B" && mkdir -p a/src/compiler && rm -rf a/src/compiler/isaspec &&
 cp -R isaspec a/src/compiler/ &&
 patch -d a -p1 --batch --forward -s < "$HERE/../../../patches/0004-isaspec-derived-bitset-params-and-override-fields.patch" &&
 cp a/src/compiler/isaspec/*.py isaspec/)
W=$B/isaspec
$PY $W/decode.py --xml $HERE/sgx545.xml --out-c $B/sgx545-isa.c --out-h $B/sgx545-isa.h
$PY $W/encode.py --xml $HERE/sgx545.xml --out-h $B/sgx545-isa-encode.h
PYTHONPATH=$W $PY $HERE/sgx545_isa_enums.py --xml $HERE/sgx545.xml --out-h $B/sgx545-isa-enums.h
CF=(-std=gnu11 -g -O1 -I $B -I $HERE -I $M/src -I $M/include -I $M/src/compiler/isaspec
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -DHAVE_TIMESPEC_GET -DHAVE_STRNDUP)
OBJS=()
for f in $B/sgx545-isa.c $HERE/sgx545_isa_encode.c $HERE/sgx545_isa_disasm.c \
         $HERE/sgx545_isa_asm.c $HERE/sgx545_isa_asm_int.c $HERE/tests/standalone_stubs.c \
         $M/src/compiler/isaspec/isaspec.c $M/src/util/ralloc.c $M/src/util/hash_table.c; do
  o=$B/obj/$(basename $f .c).o
  ${CC:-cc} "${CF[@]}" -c $f -o $o
  OBJS+=($o)
done
${CC:-cc} "${CF[@]}" $HERE/tests/sgx545_isa_test.c "${OBJS[@]}" -o $B/sgx545_isa_test
echo "$B/sgx545_isa_test"
