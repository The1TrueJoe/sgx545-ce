# Copyright © 2026 The sgx545-ce authors
# SPDX-License-Identifier: MIT

files_sgx545 = files(
  'sgx545_context.c',
  'sgx545_context.h',
  'sgx545_draw.c',
  'sgx545_program.c',
  'sgx545_program.h',
  'sgx545_public.h',
  'sgx545_resource.c',
  'sgx545_resource.h',
  'sgx545_screen.c',
  'sgx545_screen.h',
  'sgx545_state.c',
)

libsgx545 = static_library(
  'sgx545',
  files_sgx545,
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  c_args : sgx545_kernel_args,
  dependencies : [idep_nir, idep_mesautil, idep_sgx545_ws_headers, idep_sgx545_compiler],
)

driver_sgx545 = declare_dependency(
  compile_args : '-DGALLIUM_SGX545',
  link_with : [libsgx545, libsgx545winsys, libsgx545_compiler],
  dependencies : idep_nir,
)
