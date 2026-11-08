# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# The TA/3D state words (docs/STATE.md sections 2-5), described in csbgen's
# XML and packed by its generated helpers (src/imagination/csbgen).
sgx545_csbgen_py = files('../../imagination/csbgen/gen_pack_header.py')
sgx545_csb_h = []
foreach x : ['sgx545_vdm', 'sgx545_ta', 'sgx545_pbe', 'sgx545_tex']
  sgx545_csb_h += custom_target(
    x + '.h',
    input : [sgx545_csbgen_py, x + '.xml'],
    output : x + '.h',
    command : [prog_python, '@INPUT@'],
    capture : true,
  )
endforeach

idep_sgx545_csb = declare_dependency(
  include_directories : [include_directories('.'), include_directories('../../imagination')],
  sources : sgx545_csb_h,
)
