# SGX545 shader compiler, built inside a Mesa tree: link (or symlink) this
# directory as src/compiler/sgx545 and add subdir('sgx545') after glsl in
# src/compiler/meson.build (see usc/README.md).

libsgx545_compiler = static_library(
  'sgx545_compiler',
  files('usse.c', 'sgx_nir.c', 'sgx_compile.c', 'sgx_usp.c'),
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux],
  c_args : [no_override_init_args, '-Wno-missing-prototypes'],
  dependencies : [idep_nir, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

sgxc = executable(
  'sgxc',
  'sgxc.cpp',
  cpp_args : [cpp_msvc_compat_args],
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux,
                         include_directories('../glsl')],
  link_with : [libsgx545_compiler, libglsl_standalone],
  dependencies : [dep_clock, dep_thread, idep_getopt, idep_mesautil, idep_nir],
  build_by_default : true,
)
