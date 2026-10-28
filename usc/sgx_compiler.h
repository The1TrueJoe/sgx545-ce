/*
 * SGX545 shader compiler: NIR in, USSE code out, wrapped in the DDK 1.7
 * USP container (re/isa/ISA.md "The USP container") so the GLES driver's
 * finalizer (or a future gallium driver's) can link it.
 *
 * The input/output conventions are the ones the DDK's driver expects from
 * libusc (ISA.md "What a compiler needs"):
 *  - uniforms: one 32-bit constant per component, constant k of the driver's
 *    constant buffer loaded into sa[base + k] (fragment base 9, vertex 10);
 *  - fragment varyings: iterated by the PDS into pa, F32, one register per
 *    component, in the order of USP list A; a varying's iterator is its
 *    texture coordinate set + 1;
 *  - fragment colour: packed BGRA8 in a pa register (USP header 0x66-0x6c);
 *  - vertex attributes: attribute slot k at pa[4k..4k+3];
 *  - vertex outputs: position o0-o3, then the texture coordinate sets in
 *    order, each its size, then the point size.
 * Where the DDK's numbering comes from its GLSL compiler (constant numbers,
 * attribute slots), the caller can fix it with sgx_compile_options.
 */
#ifndef SGX_COMPILER_H
#define SGX_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nir_shader;
struct nir_shader_compiler_options;

#define SGX_MAX_NAMED 64
#define SGX_MAX_SETS 10

struct sgx_named {
   char name[64];
   int value;
};

struct sgx_compile_options {
   /* uniform name -> first constant number (rows/elements at +4 each);
    * uniforms not named get vec4-aligned numbers after the named ones */
   unsigned n_uniforms;
   struct sgx_named uniforms[SGX_MAX_NAMED];
   /* attribute name -> slot (pa 4*slot) */
   unsigned n_attribs;
   struct sgx_named attribs[SGX_MAX_NAMED];
   /* sampler uniform name -> texture unit */
   unsigned n_samplers;
   struct sgx_named samplers[SGX_MAX_NAMED];
   bool verbose;
};

/* varying -> texture coordinate set (sgx_assign_varyings) */
struct sgx_varying {
   char name[64];
   unsigned set;        /* first set (one per row) */
   unsigned rows, comps;
   unsigned precision;  /* GLSL: 0 none, 1 high, 2 medium, 3 low */
};

struct sgx_varyings {
   unsigned n;
   struct sgx_varying v[32];
   bool point_size;     /* set 0 reserved (vertex shaders that write gl_PointSize) */
   unsigned set_comps[SGX_MAX_SETS];
};

struct sgx_shader {
   unsigned stage;              /* MESA_SHADER_VERTEX / _FRAGMENT */
   uint8_t *usp;                /* the USP binary */
   size_t usp_size;
   uint64_t *code;              /* main program, flat (branch targets resolved) */
   unsigned n_code;
   unsigned n_alu, n_tex, n_flow; /* instruction counts */
   unsigned n_temps, n_pa;
   char *log;                   /* listing / errors (malloc'd) */
};

const struct nir_shader_compiler_options *sgx_nir_options(void);

/* GLSL ES varyings to texture coordinate sets, as the DDK's glslcompiler
 * allocates them (ISA.md "Varyings to sets"); call on each stage's NIR
 * before linking removes unused ones */
void sgx_assign_varyings(struct nir_shader *nir, bool vertex, struct sgx_varyings *out);

/* lower and compile; 0 on success */
int sgx_compile(struct nir_shader *nir, const struct sgx_compile_options *opts,
                const struct sgx_varyings *vary, struct sgx_shader *out);

void sgx_shader_free(struct sgx_shader *s);

#ifdef __cplusplus
}
#endif

#endif
