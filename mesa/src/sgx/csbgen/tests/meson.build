# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

test(
  'sgx_csb',
  executable(
    'sgx_csb_test',
    'sgx_csb_test.c',
    include_directories : [inc_include, inc_src],
    dependencies : [idep_sgx_csb],
  ),
  suite : ['sgx'],
)
