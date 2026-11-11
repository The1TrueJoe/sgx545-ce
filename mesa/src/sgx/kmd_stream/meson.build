# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# The command streams powervr_sgx reads, defined in its uAPI's stream XML, and
# the packers gen_stream.py writes from them (powervr_sgx_kmd_stream.h).

sgx_kmd_stream_h = custom_target(
  'powervr_sgx_kmd_stream.h',
  input : ['gen_stream.py', 'powervr_sgx_kmd_stream.xml'],
  output : 'powervr_sgx_kmd_stream.h',
  command : [prog_python, '@INPUT0@', 'user', '@INPUT1@'],
  capture : true,
)

idep_sgx_kmd_stream = declare_dependency(
  include_directories : include_directories('.'),
  sources : sgx_kmd_stream_h,
)
