# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# SGX545 PDS programs: sgx_pds.xml through
# isaspec into an encoder and a disassembler, plus a text assembler.
# (The generated files are named sgxpds-isa.*: isaspec names its
# functions after the part of the file name before the first '-'.)

sgx_pds_depend_files = [files('sgx_pds.xml'), isaspec_py_deps]

sgx_pds_decode = custom_target(
  'sgxpds-isa',
  input : 'sgx_pds.xml',
  output : ['sgxpds-isa.c', 'sgxpds-isa.h'],
  command : [prog_isaspec_decode, '--xml', '@INPUT@',
             '--out-c', '@OUTPUT0@', '--out-h', '@OUTPUT1@'],
  depend_files : sgx_pds_depend_files,
)

sgx_pds_encode_h = custom_target(
  'sgxpds-isa-encode.h',
  input : 'sgx_pds.xml',
  output : 'sgxpds-isa-encode.h',
  command : [prog_isaspec_encode, '--xml', '@INPUT@', '--out-h', '@OUTPUT@'],
  depend_files : sgx_pds_depend_files,
)

sgx_pds_enums_h = custom_target(
  'sgxpds-isa-enums.h',
  input : 'sgx_pds.xml',
  output : 'sgxpds-isa-enums.h',
  command : [prog_python, files('../isa/sgx_isa_enums.py'), '--xml', '@INPUT@',
             '--out-h', '@OUTPUT@', '--prefix', 'sgx_pds', '--values', ''],
  depend_files : [sgx_pds_depend_files, files('../isa/sgx_isa_enums.py')],
)

libsgx_pds = static_library(
  'sgx_pds',
  [sgx_pds_decode, sgx_pds_encode_h, sgx_pds_enums_h,
   'sgx_pds_encode.c', 'sgx_pds_disasm.c', 'sgx_pds_asm.c'],
  include_directories : [inc_include, inc_src, include_directories('.')],
  # the decoder's tables for the encode-only data-word roots are unused
  c_args : cc.get_supported_arguments('-Wno-unused-variable'),
  dependencies : [idep_mesautil, idep_isaspec_decode],
  link_with : [libisaspec],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

idep_sgx_pds = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libsgx_pds,
  sources : [sgx_pds_enums_h],
)

if with_tests
  subdir('tests')
endif
