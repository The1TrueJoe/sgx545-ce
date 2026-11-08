# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# Text -> instruction -> words -> text over each list; a line that gives
# words must encode to them.
test(
  'sgx_pds',
  executable(
    'sgx_pds_test',
    'sgx_pds_test.c',
    include_directories : [inc_include, inc_src],
    dependencies : [idep_sgx_pds, idep_mesautil],
  ),
  args : files('pds.txt'),
  suite : ['sgx'],
)
