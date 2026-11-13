# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# The per-core descriptions (sgx_core.h): one entry per SGX core.
libsgx_core = static_library(
  'sgx_core',
  files('sgx_core.c'),
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

idep_sgx_core = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libsgx_core,
)
