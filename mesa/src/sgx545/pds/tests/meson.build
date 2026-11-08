# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# Text -> instruction -> words -> text over each list; a line that gives
# words must encode to them.
test(
  'sgx545_pds',
  executable(
    'sgx545_pds_test',
    'sgx545_pds_test.c',
    include_directories : [inc_include, inc_src],
    dependencies : [idep_sgx545_pds, idep_mesautil],
  ),
  args : files('pds.txt'),
  suite : ['sgx545'],
)
