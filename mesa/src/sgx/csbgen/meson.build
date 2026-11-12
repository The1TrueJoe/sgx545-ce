# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# The TA/3D state words, described in csbgen's XML and packed by the helpers
# its generator (src/imagination/csbgen) writes: csbgen/<name>.h for each
# <name>.xml, as src/imagination/csbgen/rogue does.

sgx_csb_xml_files = [
  'pbe.xml',
  'ta.xml',
  'tex.xml',
  'vdm.xml',
]

sgx_csb_h = []
foreach f : sgx_csb_xml_files
  _name = '@0@.h'.format(f.split('.')[0])
  sgx_csb_h += custom_target(
    _name,
    input : [pvr_gen_pack_header_py, f],
    output : _name,
    command : [prog_python, '@INPUT@'],
    capture : true,
  )
endforeach

idep_sgx_csb = declare_dependency(
  include_directories : [include_directories('.', '..'), inc_imagination],
  sources : sgx_csb_h,
)

if with_tests
  subdir('tests')
endif
