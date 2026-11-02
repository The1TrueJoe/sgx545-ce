# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# SGX545 USSE instruction set: sgx545.xml (written from docs/ISA.md) through
# isaspec into an encoder, a disassembler and a text assembler.

sgx545_isa_depend_files = [files('sgx545.xml'), isaspec_py_deps]

sgx545_isa_decode = custom_target(
  'sgx545-isa',
  input : 'sgx545.xml',
  output : ['sgx545-isa.c', 'sgx545-isa.h'],
  command : [prog_isaspec_decode, '--xml', '@INPUT@',
             '--out-c', '@OUTPUT0@', '--out-h', '@OUTPUT1@'],
  depend_files : sgx545_isa_depend_files,
)

sgx545_isa_encode_h = custom_target(
  'sgx545-isa-encode.h',
  input : 'sgx545.xml',
  output : 'sgx545-isa-encode.h',
  command : [prog_isaspec_encode, '--xml', '@INPUT@', '--out-h', '@OUTPUT@'],
  depend_files : sgx545_isa_depend_files,
)

sgx545_isa_enums_h = custom_target(
  'sgx545-isa-enums.h',
  input : 'sgx545.xml',
  output : 'sgx545-isa-enums.h',
  command : [prog_python, files('sgx545_isa_enums.py'), '--xml', '@INPUT@',
             '--out-h', '@OUTPUT@'],
  depend_files : [sgx545_isa_depend_files, files('sgx545_isa_enums.py')],
)

libsgx545_isa = static_library(
  'sgx545_isa',
  [sgx545_isa_decode, sgx545_isa_encode_h, sgx545_isa_enums_h,
   'sgx545_isa_encode.c', 'sgx545_isa_disasm.c', 'sgx545_isa_asm.c',
   'sgx545_isa_asm_int.c'],
  include_directories : [inc_include, inc_src, include_directories('.')],
  dependencies : [idep_mesautil, idep_isaspec_decode],
  link_with : [libisaspec],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

idep_sgx545_isa = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libsgx545_isa,
  sources : [sgx545_isa_enums_h],
)

if with_tests
  test(
    'sgx545_isa',
    executable(
      'sgx545_isa_test',
      'tests/sgx545_isa_test.c',
      include_directories : [inc_include, inc_src],
      dependencies : [idep_sgx545_isa, idep_mesautil],
    ),
    args : files('tests/gles2.txt'),
    suite : ['sgx545'],
  )
endif
