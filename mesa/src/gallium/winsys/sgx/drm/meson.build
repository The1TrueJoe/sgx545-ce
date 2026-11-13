# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# The driver's view of the winsys: sgxws.h alone.  No services or kernel
# headers on its include path, so nothing from kernel/src can reach the
# gallium driver.
idep_sgx_ws = declare_dependency(
  include_directories : include_directories('.'),
  dependencies : [idep_sgx_pds, idep_sgx_csb, idep_sgx_core],
)

# The powervr_sgx backend: the uAPI header (include/drm-uapi), its command
# structs included.
sgx_ws_files = files('sgx_frame.c', 'sgx_powervr_sgx.c')
sgx_ws_args = []
sgx_ws_inc = [include_directories('.')]

libsgxwinsys = static_library(
  'sgxwinsys',
  sgx_ws_files, 'sgx_drm_winsys.c',
  include_directories : [
    sgx_ws_inc, inc_src, inc_include,
    inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  c_args : sgx_ws_args,
  dependencies : [idep_mesautil, dep_libdrm, idep_sgx_pds, idep_sgx_csb,
                  idep_sgx_core, idep_sgx_compiler],
  gnu_symbol_visibility : 'hidden',
)

# The winsys on its own, without the gallium driver: a hardware check of the
# kernel interface, built only when asked for (ninja sgx_frame_test).
sgx_frame_test = executable(
  'sgx_frame_test',
  files('sgx_frame_test.c'), sgx_ws_files,
  include_directories : [sgx_ws_inc, inc_src, inc_include],
  c_args : sgx_ws_args,
  dependencies : [idep_mesautil, dep_libdrm, dep_m, idep_sgx_pds, idep_sgx_csb,
                  idep_sgx_core, idep_sgx_compiler],
  build_by_default : false,
  install : false,
)

# Hand-written fragment programs through the winsys, their result read back:
# the hardware checks on the open stack (ninja sgx_hwt; tools/isa/hwt_open.py).
sgx_hwt = executable(
  'sgx_hwt',
  files('sgx_hwt.c'), sgx_ws_files,
  include_directories : [sgx_ws_inc, inc_src, inc_include],
  c_args : sgx_ws_args,
  dependencies : [idep_mesautil, dep_libdrm, dep_m, idep_sgx_pds, idep_sgx_csb,
                  idep_sgx_isa, idep_sgx_core,
                  idep_sgx_compiler],
  build_by_default : false,
  install : false,
)
