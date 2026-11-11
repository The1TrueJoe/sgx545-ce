# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# The driver's view of the winsys: sgxws.h alone.  No services or kernel
# headers on its include path, so nothing from kernel/src can reach the
# gallium driver.
idep_sgx_ws = declare_dependency(
  include_directories : include_directories('.'),
  dependencies : [idep_sgx_pds, idep_sgx_csb, idep_sgx_core],
)

# The powervr_sgx backend: the uAPI header (include/drm-uapi) and the stream
# packers generated from its stream definitions (src/sgx/kmd_stream).
sgx_ws_files = files('sgx_frame.c', 'sgx_powervr_sgx.c')
sgx_ws_args = []
sgx_ws_inc = [include_directories('.')]

# The services backend (pvrsrvkm, the DDK's kernel driver), with
# -Dgallium-sgx-pvrsrvkm (patch 0007): mesa/build.sh links powervr_sgx's libpvr
# into this directory for it.  It is built against the
# kernel driver's own bridge headers (kernel/src, linked into the tree as
# include/sgx-kernel) with the kernel's defines (libpvr/abi/flags.sh);
# nothing else in the winsys sees them.
if get_option('gallium-sgx-pvrsrvkm')
  inc_sgx_kernel = include_directories(
    '../../../../../include/sgx-kernel/include4',
    '../../../../../include/sgx-kernel/services4/include',
    '../../../../../include/sgx-kernel/services4/include/env/linux',
    '../../../../../include/sgx-kernel/services4/srvkm/hwdefs',
    '../../../../../include/sgx-kernel/services4/srvkm/devices/sgx',
    '../../../../../include/sgx-kernel/services4/srvkm/include',
    '../../../../../include/sgx-kernel/services4/system/ce5300',
    '../../../../../include/sgx-kernel/services4/system/include',
    '../../../../../include/sgx-kernel/services4/system/unified',
  )

  sgx_kernel_args = [
    '-DLINUX', '-DSGX545', '-DSUPPORT_SGX545', '-DSGX_CORE_REV=1013', '-DSERVICES4',
    '-DSUPPORT_SRVINIT', '-DSUPPORT_SGX', '-DTRANSFER_QUEUE', '-DSYS_USING_INTERRUPTS',
    '-DSUPPORT_HW_RECOVERY', '-DPVR_SECURE_HANDLES', '-DUSE_PTHREADS',
    '-DSUPPORT_SGX_EVENT_OBJECT', '-DSUPPORT_SGX_HWPERF', '-DSUPPORT_LINUX_X86_PAT',
    '-DSUPPORT_CACHE_LINE_FLUSH', '-DSUPPORT_CPU_CACHED_BUFFERS', '-DDISABLE_PM',
    '-DSUPPORT_SGX_NEW_STATUS_VALS', '-DSUPPORT_PERCONTEXT_PB', '-DRELEASE',
    '-DSUPPORT_DRI_DRM', '-DSUPPORT_LINUX_X86_WRITECOMBINE',
    '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-missing-field-initializers',
  ]

  libsgxpvr = static_library(
    'sgxpvr',
    files('libpvr/pvr.c', 'sgx_pvr.c'),
    include_directories : [include_directories('.', 'libpvr'), inc_sgx_kernel, inc_src, inc_include],
    c_args : sgx_kernel_args,
    dependencies : [idep_sgx_pds, idep_sgx_core],
    gnu_symbol_visibility : 'hidden',
  )
  sgx_ws_args += '-DSGX_PVRSRVKM'
  sgx_ws_pvr = libsgxpvr.extract_all_objects(recursive : false)
else
  sgx_ws_pvr = []
endif

libsgxwinsys = static_library(
  'sgxwinsys',
  sgx_ws_files, 'sgx_drm_winsys.c',
  include_directories : [
    sgx_ws_inc, inc_src, inc_include,
    inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  c_args : sgx_ws_args,
  objects : sgx_ws_pvr,
  dependencies : [idep_mesautil, dep_libdrm, idep_sgx_pds, idep_sgx_csb,
                  idep_sgx_core, idep_sgx_kmd_stream],
  gnu_symbol_visibility : 'hidden',
)

# The winsys on its own, without the gallium driver: a hardware check of the
# kernel interface, built only when asked for (ninja sgx_frame_test).
sgx_frame_test = executable(
  'sgx_frame_test',
  files('sgx_frame_test.c'), sgx_ws_files,
  include_directories : [sgx_ws_inc, inc_src, inc_include],
  c_args : sgx_ws_args,
  objects : sgx_ws_pvr,
  dependencies : [idep_mesautil, dep_libdrm, dep_m, idep_sgx_pds, idep_sgx_csb,
                  idep_sgx_core, idep_sgx_kmd_stream],
  build_by_default : false,
  install : false,
)
