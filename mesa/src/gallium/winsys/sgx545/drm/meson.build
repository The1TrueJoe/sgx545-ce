# Copyright © 2026 The sgx545-ce authors
# SPDX-License-Identifier: MIT

# The services client is sgx545-ce's libpvr/ (linked into this directory as
# libpvr by mesa/build.sh); the bridge structures come from the kernel
# driver's own headers (kernel/src, linked into the tree as
# include/sgx545-kernel), built with the kernel's defines
# (libpvr/abi/flags.sh).
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

idep_sgx545_ws_headers = declare_dependency(
  include_directories : [include_directories('.', 'libpvr'), inc_sgx545_kernel],
  compile_args : sgx545_kernel_args,
)

libsgx545winsys = static_library(
  'sgx545winsys',
  files('libpvr/pvr.c', 'sgx545_frame.c', 'sgx545_drm_winsys.c'),
  include_directories : [
    inc_src, inc_include,
    inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  c_args : sgx545_kernel_args,
  dependencies : [idep_mesautil, idep_sgx545_ws_headers],
  gnu_symbol_visibility : 'hidden',
)

# the winsys on its own, no Mesa: mesa/build.sh runs it on the EA
sgx545_frame_test = executable(
  'sgx545_frame_test',
  files('sgx545_frame_test.c', 'libpvr/pvr.c', 'sgx545_frame.c'),
  c_args : sgx545_kernel_args,
  dependencies : [idep_sgx545_ws_headers, dep_m],
  build_by_default : true,
  install : false,
)
