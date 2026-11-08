# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# SGX545 shader compiler: NIR -> USSE, encoded through the isaspec
# encoder in ../isa.

libsgx545_compiler = static_library(
  'sgx545_compiler',
  files('sgx_forms.c', 'sgx_nir.c', 'sgx_compile.c', 'sgx_program.c'),
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux],
  c_args : [no_override_init_args],
  dependencies : [idep_nir, idep_mesautil, idep_sgx545_isa],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

idep_sgx545_compiler = declare_dependency(
  link_with : [libsgx545_compiler, libsgx545_isa],
  include_directories : include_directories('.'),
)

if with_tests
  test(
    'sgx545_compiler',
    executable(
      'sgx545_compiler_test',
      files('tests/sgx_compile_test.cpp'),
      cpp_args : [cpp_msvc_compat_args],
      include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux],
      dependencies : [idep_gtest, idep_nir, idep_mesautil, idep_sgx545_isa, idep_sgx545_compiler],
    ),
    suite : ['sgx545'],
    protocol : 'gtest',
  )
endif
