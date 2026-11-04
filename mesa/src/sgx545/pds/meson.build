# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# SGX545 PDS programs: sgx545_pds.xml (written from docs/PDS.md) through
# isaspec into an encoder and a disassembler, plus a text assembler.
# (The generated files are named sgx545pds-isa.*: isaspec names its
# functions after the part of the file name before the first '-'.)

sgx545_pds_depend_files = [files('sgx545_pds.xml'), isaspec_py_deps]

sgx545_pds_decode = custom_target(
  'sgx545pds-isa',
  input : 'sgx545_pds.xml',
  output : ['sgx545pds-isa.c', 'sgx545pds-isa.h'],
  command : [prog_isaspec_decode, '--xml', '@INPUT@',
             '--out-c', '@OUTPUT0@', '--out-h', '@OUTPUT1@'],
  depend_files : sgx545_pds_depend_files,
)

sgx545_pds_encode_h = custom_target(
  'sgx545pds-isa-encode.h',
  input : 'sgx545_pds.xml',
  output : 'sgx545pds-isa-encode.h',
  command : [prog_isaspec_encode, '--xml', '@INPUT@', '--out-h', '@OUTPUT@'],
  depend_files : sgx545_pds_depend_files,
)

sgx545_pds_enums_h = custom_target(
  'sgx545pds-isa-enums.h',
  input : 'sgx545_pds.xml',
  output : 'sgx545pds-isa-enums.h',
  command : [prog_python, files('../isa/sgx545_isa_enums.py'), '--xml', '@INPUT@',
             '--out-h', '@OUTPUT@', '--prefix', 'sgx545_pds', '--values', ''],
  depend_files : [sgx545_pds_depend_files, files('../isa/sgx545_isa_enums.py')],
)

libsgx545_pds = static_library(
  'sgx545_pds',
  [sgx545_pds_decode, sgx545_pds_encode_h, sgx545_pds_enums_h,
   'sgx545_pds_encode.c', 'sgx545_pds_disasm.c', 'sgx545_pds_asm.c'],
  include_directories : [inc_include, inc_src, include_directories('.')],
  # the decoder's tables for the encode-only data-word roots are unused
  c_args : cc.get_supported_arguments('-Wno-unused-variable'),
  dependencies : [idep_mesautil, idep_isaspec_decode],
  link_with : [libisaspec],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

idep_sgx545_pds = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libsgx545_pds,
  sources : [sgx545_pds_enums_h],
)

if with_tests
  test(
    'sgx545_pds',
    executable(
      'sgx545_pds_test',
      'tests/sgx545_pds_test.c',
      include_directories : [inc_include, inc_src],
      dependencies : [idep_sgx545_pds, idep_mesautil],
    ),
    args : files('tests/pds.txt'),
    suite : ['sgx545'],
  )
endif
