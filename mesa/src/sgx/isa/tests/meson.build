# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# Text -> instruction -> words -> text over each list; a line that gives
# words must encode to them.
test(
  'sgx_isa',
  executable(
    'sgx_isa_test',
    'sgx_isa_test.c',
    include_directories : [inc_include, inc_src],
    dependencies : [idep_sgx_isa, idep_mesautil],
  ),
  args : files('gles2.txt', 'int.txt', 'coissue.txt'),
  suite : ['sgx'],
)

# sgx-asm and sgx-disasm over the same lists, in batch, hex and binary.
test(
  'sgx_isa_tools',
  prog_python,
  args : [files('tools_test.py'), sgx_asm, sgx_disasm,
          files('gles2.txt', 'int.txt', 'coissue.txt')],
  suite : ['sgx'],
)
