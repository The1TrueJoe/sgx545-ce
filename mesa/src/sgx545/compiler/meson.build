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
