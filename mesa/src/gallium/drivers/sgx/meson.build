# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

files_sgx = files(
  'sgx_context.c',
  'sgx_context.h',
  'sgx_draw.c',
  'sgx_program.c',
  'sgx_program.h',
  'sgx_public.h',
  'sgx_resource.c',
  'sgx_resource.h',
  'sgx_screen.c',
  'sgx_screen.h',
  'sgx_state.c',
  'sgx_texture.c',
  'sgx_texture.h',
)

libsgx = static_library(
  'sgx',
  files_sgx,
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  dependencies : [idep_nir, idep_mesautil, idep_sgx_ws, idep_sgx_compiler,
                  idep_sgx_core],
)

driver_sgx = declare_dependency(
  compile_args : '-DGALLIUM_SGX',
  link_with : [libsgx, libsgxwinsys, libsgx_compiler, libsgx_core],
  dependencies : idep_nir,
)
