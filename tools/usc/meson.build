# sgxc, the standalone SGX545 compiler (Mesa's GLSL frontend + the backend in
# mesa/src/sgx545/compiler). Built inside a Mesa tree after src/compiler (it
# needs libglsl_standalone); see README.md.

sgxc = executable(
  'sgxc',
  'sgxc.cpp',
  cpp_args : [cpp_msvc_compat_args],
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux,
                         inc_glsl],
  link_with : [libglsl_standalone],
  dependencies : [dep_clock, dep_thread, idep_getopt, idep_mesautil, idep_nir, idep_sgx545_compiler],
  build_by_default : true,
)
