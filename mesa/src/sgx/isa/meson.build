# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# SGX545 USSE instruction set: sgx.xml (written from docs/ISA.md) through
# isaspec into an encoder, a disassembler and a text assembler.

sgx_isa_depend_files = [files('sgx.xml'), isaspec_py_deps]

sgx_isa_decode = custom_target(
  'sgx-isa',
  input : 'sgx.xml',
  output : ['sgx-isa.c', 'sgx-isa.h'],
  command : [prog_isaspec_decode, '--xml', '@INPUT@',
             '--out-c', '@OUTPUT0@', '--out-h', '@OUTPUT1@'],
  depend_files : sgx_isa_depend_files,
)

sgx_isa_encode_h = custom_target(
  'sgx-isa-encode.h',
  input : 'sgx.xml',
  output : 'sgx-isa-encode.h',
  command : [prog_isaspec_encode, '--xml', '@INPUT@', '--out-h', '@OUTPUT@'],
  depend_files : sgx_isa_depend_files,
)

sgx_isa_enums_h = custom_target(
  'sgx-isa-enums.h',
  input : 'sgx.xml',
  output : 'sgx-isa-enums.h',
  command : [prog_python, files('sgx_isa_enums.py'), '--xml', '@INPUT@',
             '--out-h', '@OUTPUT@'],
  depend_files : [sgx_isa_depend_files, files('sgx_isa_enums.py')],
)

libsgx_isa = static_library(
  'sgx_isa',
  [sgx_isa_decode, sgx_isa_encode_h, sgx_isa_enums_h,
   'sgx_isa_encode.c', 'sgx_isa_disasm.c', 'sgx_isa_asm.c',
   'sgx_isa_asm_int.c', 'sgx_isa_coissue.c'],
  include_directories : [inc_include, inc_src, include_directories('.')],
  dependencies : [idep_mesautil, idep_isaspec_decode],
  link_with : [libisaspec],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

idep_sgx_isa = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libsgx_isa,
  sources : [sgx_isa_enums_h],
)

sgx_asm = executable(
  'sgx-asm',
  'assembler.c',
  include_directories : [inc_include, inc_src],
  dependencies : [idep_sgx_isa, idep_mesautil],
  build_by_default : with_tools.contains('sgx'),
  install : false,
)

sgx_disasm = executable(
  'sgx-disasm',
  'disasm.c',
  include_directories : [inc_include, inc_src],
  dependencies : [idep_sgx_isa, idep_mesautil],
  build_by_default : with_tools.contains('sgx'),
  install : false,
)

if with_tests
  subdir('tests')
endif
