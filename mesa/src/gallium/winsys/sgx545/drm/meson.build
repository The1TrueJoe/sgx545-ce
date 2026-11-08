# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

fs = import('fs')

# The driver's view of the winsys: sgxws.h alone.  No services or kernel
# headers on its include path, so nothing from kernel/src can reach the
# gallium driver (AGENTS.md section 3.5).
idep_sgx545_ws = declare_dependency(
  include_directories : include_directories('.'),
  dependencies : [idep_sgx545_pds, idep_sgx545_csb],
)

# The powervr_sgx backend: the uAPI header (include/drm-uapi) and the stream
# packers generated from its stream definitions (powervr_sgx_kmd_stream.h,
# docs/uapi/gen_stream.py).
sgx545_ws_files = files('sgx545_frame.c', 'sgx545_powervr_sgx.c')
sgx545_ws_args = []
sgx545_ws_inc = [include_directories('.')]

# The services backend (pvrsrvkm, openHC's stack): only where mesa/build.sh
# links sgx545-ce's libpvr into this directory.  It is built against the
# kernel driver's own bridge headers (kernel/src, linked into the tree as
# include/sgx545-kernel) with the kernel's defines (libpvr/abi/flags.sh);
# nothing else in the winsys sees them.
if fs.exists('libpvr/pvr.c')
  inc_sgx545_kernel = include_directories(
    '../../../../../include/sgx545-kernel/include4',
    '../../../../../include/sgx545-kernel/services4/include',
    '../../../../../include/sgx545-kernel/services4/include/env/linux',
    '../../../../../include/sgx545-kernel/services4/srvkm/hwdefs',
    '../../../../../include/sgx545-kernel/services4/srvkm/devices/sgx',
    '../../../../../include/sgx545-kernel/services4/srvkm/include',
    '../../../../../include/sgx545-kernel/services4/system/ce5300',
    '../../../../../include/sgx545-kernel/services4/system/include',
    '../../../../../include/sgx545-kernel/services4/system/unified',
  )

  sgx545_kernel_args = [
    '-DLINUX', '-DSGX545', '-DSUPPORT_SGX545', '-DSGX_CORE_REV=1013', '-DSERVICES4',
    '-DSUPPORT_SRVINIT', '-DSUPPORT_SGX', '-DTRANSFER_QUEUE', '-DSYS_USING_INTERRUPTS',
    '-DSUPPORT_HW_RECOVERY', '-DPVR_SECURE_HANDLES', '-DUSE_PTHREADS',
    '-DSUPPORT_SGX_EVENT_OBJECT', '-DSUPPORT_SGX_HWPERF', '-DSUPPORT_LINUX_X86_PAT',
    '-DSUPPORT_CACHE_LINE_FLUSH', '-DSUPPORT_CPU_CACHED_BUFFERS', '-DDISABLE_PM',
    '-DSUPPORT_SGX_NEW_STATUS_VALS', '-DSUPPORT_PERCONTEXT_PB', '-DRELEASE',
    '-DSUPPORT_DRI_DRM', '-DSUPPORT_LINUX_X86_WRITECOMBINE',
    '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-missing-field-initializers',
  ]

  libsgx545pvr = static_library(
    'sgx545pvr',
    files('libpvr/pvr.c', 'sgx545_pvr.c'),
    include_directories : [include_directories('.', 'libpvr'), inc_sgx545_kernel, inc_src, inc_include],
    c_args : sgx545_kernel_args,
    dependencies : idep_sgx545_pds,
    gnu_symbol_visibility : 'hidden',
  )
  sgx545_ws_args += '-DSGX545_PVRSRVKM'
  sgx545_ws_pvr = libsgx545pvr.extract_all_objects(recursive : false)
else
  sgx545_ws_pvr = []
endif

libsgx545winsys = static_library(
  'sgx545winsys',
  sgx545_ws_files, 'sgx545_drm_winsys.c',
  include_directories : [
    sgx545_ws_inc, inc_src, inc_include,
    inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  c_args : sgx545_ws_args,
  objects : sgx545_ws_pvr,
  dependencies : [idep_mesautil, dep_libdrm, idep_sgx545_pds, idep_sgx545_csb],
  gnu_symbol_visibility : 'hidden',
)

# the winsys on its own, no Mesa: mesa/build.sh runs it on the EA
sgx545_frame_test = executable(
  'sgx545_frame_test',
  files('sgx545_frame_test.c'), sgx545_ws_files,
  include_directories : [sgx545_ws_inc, inc_src, inc_include],
  c_args : sgx545_ws_args,
  objects : sgx545_ws_pvr,
  dependencies : [idep_mesautil, dep_libdrm, dep_m, idep_sgx545_pds, idep_sgx545_csb],
  build_by_default : true,
  install : false,
)
