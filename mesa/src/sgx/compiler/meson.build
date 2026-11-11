# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# SGX545 shader compiler: NIR -> USSE, encoded through the isaspec
# encoder in ../isa.

libsgx_compiler = static_library(
  'sgx_compiler',
  files('sgx_forms.c', 'sgx_nir.c', 'sgx_compile.c', 'sgx_program.c',
        'sgx_fixed.c'),
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux],
  c_args : [no_override_init_args],
  dependencies : [idep_nir, idep_mesautil, idep_sgx_isa],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

idep_sgx_compiler = declare_dependency(
  link_with : [libsgx_compiler, libsgx_isa],
  include_directories : include_directories('.'),
)

if with_tests
  test(
    'sgx_compiler',
    executable(
      'sgx_compiler_test',
      files('tests/sgx_compile_test.cpp'),
      cpp_args : [cpp_msvc_compat_args],
      include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux],
      dependencies : [idep_gtest, idep_nir, idep_mesautil, idep_sgx_isa, idep_sgx_compiler],
    ),
    suite : ['sgx'],
    protocol : 'gtest',
  )
endif
