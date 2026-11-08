# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

test(
  'sgx545_csb',
  executable(
    'sgx545_csb_test',
    'sgx545_csb_test.c',
    include_directories : [inc_include, inc_src],
    dependencies : [idep_sgx545_csb],
  ),
  suite : ['sgx545'],
)
