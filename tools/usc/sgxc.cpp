/*
 * sgxc: compile a GLSL ES 1.00 program with Mesa's GLSL frontend and the
 * SGX545 backend, standalone (no GL context, no driver).
 *
 *   sgxc [-v] [-u name=k]... [-a name=slot]... [-s name=unit]... -o prefix a.vert b.frag
 *
 * Writes prefix.vert.usp / prefix.frag.usp (USP binaries), prefix.<stage>.code
 * (the main program's words as "hi lo" hex lines) and prefix.<stage>.txt (the
 * compiler's log). -u/-a/-s pin uniform constant numbers, attribute slots and
 * sampler units (the DDK's GLSL compiler chooses these; see sgx_compiler.h).
 * A name prefixed "vert:" or "frag:" applies to that stage only.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main/mtypes.h"
#include "nir.h"
#include "nir_builder.h"
#include "gl_nir_linker.h"
#include "linker_util.h"
#include "standalone.h"
#include "standalone_scaffolding.h"

#include "sgx_compiler.h"
#include "sgx_usp.h"

/* -u / -a / -s pins: glslcompiler's constant numbers, attribute slots and
 * sampler units (calib.py measures them), handed to the compiler as its
 * layout (struct sgx_layout); names not pinned are the compiler's, except
 * samplers: glslcompiler numbers those in order of use (corpus c3
 * tx_nd_s1: s1 alone is sampler 0; tx_nd_two: s, s1 = 0, 1) */
#define MAX_NAMED 256

struct named {
   char name[64];
   int value;
};

struct pins {
   unsigned n[3];                       /* by enum sgx_layout_kind */
   struct named t[3][MAX_NAMED];
   struct named used[16];               /* unpinned samplers, in order of use */
   unsigned n_used;
};

static void
named(struct pins *p, enum sgx_layout_kind k, const char *arg)
{
   const char *eq = strchr(arg, '=');
   if (!eq || p->n[k] >= MAX_NAMED || eq - arg >= 63) {
      fprintf(stderr, "sgxc: bad name=value '%s'\n", arg);
      exit(2);
   }
   struct named *e = &p->t[k][p->n[k]++];
   memcpy(e->name, arg, eq - arg);
   e->name[eq - arg] = 0;
   e->value = atoi(eq + 1);
}

/* past every pin: a uniform pin takes a vec4 (rows and elements are pinned one by one) */
static unsigned
pins_first_free(void *data, enum sgx_layout_kind k)
{
   const struct pins *p = (const struct pins *)data;
   unsigned next = 0;
   for (unsigned i = 0; i < p->n[k]; i++)
      next = MAX2(next, (unsigned)p->t[k][i].value + (k == SGX_LAYOUT_UNIFORM ? 4 : 1));
   return next;
}

static int
pins_place(void *data, enum sgx_layout_kind k, const char *name)
{
   struct pins *p = (struct pins *)data;
   for (unsigned i = 0; i < p->n[k]; i++)
      if (!strcmp(p->t[k][i].name, name))
         return p->t[k][i].value;
   if (k != SGX_LAYOUT_SAMPLER)
      return -1;
   for (unsigned i = 0; i < p->n_used; i++)
      if (!strcmp(p->used[i].name, name))
         return p->used[i].value;
   if (p->n_used == 16 || strlen(name) >= sizeof(p->used[0].name))
      return -1;
   struct named *e = &p->used[p->n_used];
   strcpy(e->name, name);
   e->value = pins_first_free(p, k) + p->n_used++;
   return e->value;
}

static void
write_file(const char *prefix, const char *suffix, const void *d, size_t n)
{
   char path[1024];
   snprintf(path, sizeof path, "%s.%s", prefix, suffix);
   FILE *f = fopen(path, "wb");
   if (!f || fwrite(d, 1, n, f) != n) {
      perror(path);
      exit(1);
   }
   fclose(f);
}

static unsigned
vec4_slots(const struct glsl_type *type, bool bindless)
{
   return glsl_count_vec4_slots(type, false, bindless);
}

/* the gallium driver's (sgx_program.c): load_uniform base and offset from
 * vec4 slots to components */
static bool
rescale_uniform(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_load_uniform)
      return false;
   b->cursor = nir_before_instr(&in->instr);
   nir_def *off = nir_src_is_const(in->src[0]) ? nir_imm_int(b, 0) : nir_imul_imm(b, in->src[0].ssa, 4);
   unsigned base = nir_intrinsic_base(in) + (nir_src_is_const(in->src[0]) ? nir_src_as_uint(in->src[0]) : 0);
   /* and a 16-bit load as an f32 one plus f2f16: the driver doesn't set
    * glsl_16bit_load_dst, so its mediump uniforms arrive that way */
   bool h = in->def.bit_size == 16;
   nir_def *v = nir_load_uniform(b, in->def.num_components, 32, off, .base = (int)(base * 4),
                                 .range = nir_intrinsic_range(in) * 4,
                                 .dest_type = h ? nir_type_float32 : nir_intrinsic_dest_type(in));
   nir_def_replace(&in->def, h ? nir_f2f16(b, v) : v);
   return true;
}

int
main(int argc, char **argv)
{
   struct sgx_compile_options opts;
   static struct pins pins;
   const char *prefix = "out";
   int c;

   memset(&opts, 0, sizeof(opts));
   bool lowp16 = false, driver_uniforms = false;
   unsigned glsl_version = 100;
   while ((c = getopt(argc, argv, "vpFLDMHPJG:B:u:a:s:o:V:")) != -1) {
      switch (c) {
      case 'F': opts.front_sa = 5; break;   /* gl_FrontFacing as the gallium driver reads it */
      case 'L': driver_uniforms = true; break;
      case 'D': opts.driver_consts = opts.pds_samples_alone = true; break; /* our driver: its constant formats, PDS samples without the plain iteration */
      case 'M': opts.fs_mediump = true; break;      /* fragment floats mediump (st's fixed function) */
      case 'B': opts.tex_bgra = strtoul(optarg, NULL, 0); break; /* units whose texels arrive BGRA-ordered */
      case 'H': opts.f16_samples = true; break;  /* mediump texels returned as F16 */
      case 'P': opts.split_phases = true; break; /* discard test and colour in two phases */
      case 'J': opts.dep_f16_samples = true; break; /* dependent mediump texels returned F16 too */
      case 'G': glsl_version = atoi(optarg); break; /* GLSL version: 100 ES (default), 110-130 desktop compat */
      case 'V': {                                   /* -V slot=fmt: attribute slot's raw format (sgx_vtx_fmt) */
         unsigned slot, fmt;
         if (sscanf(optarg, "%u=%u", &slot, &fmt) == 2 && slot < 16)
            opts.vtx_fmt[slot] = fmt;
         break;
      }
      case 'p': lowp16 = true; opts.f16 = true; break;
      case 'v': opts.verbose = true; break;
      case 'u': named(&pins, SGX_LAYOUT_UNIFORM, optarg); break;
      case 'a': named(&pins, SGX_LAYOUT_ATTRIB, optarg); break;
      case 's': named(&pins, SGX_LAYOUT_SAMPLER, optarg); break;
      case 'o': prefix = optarg; break;
      default:
         fprintf(stderr, "usage: sgxc [-v] [-p] [-L] [-D] [-M] [-G version] [-B unitmask] [-H] [-P] [-V slot=fmt] [-u name=k] [-a name=slot] [-s name=unit] -o prefix a.vert b.frag\n");
         return 2;
      }
   }

   /* GLSL 1.30 and later (and ES 3.00): integers in the integer units */
   opts.native_integers = glsl_version >= 130;
   struct standalone_options so;
   memset(&so, 0, sizeof(so));
   so.glsl_version = glsl_version;
   so.do_link = 0;
   so.lower_precision = lowp16;   /* -p: mediump/lowp arithmetic in float16 */

   struct gl_context local_ctx;
   struct gl_context *ctx = &local_ctx;
   struct gl_shader_program *prog =
      standalone_compile_shader(&so, argc - optind, &argv[optind], ctx);
   if (!prog) {
      fprintf(stderr, "sgxc: GLSL compile failed\n");
      return 1;
   }

   /* varying sets, per stage, from each shader as compiled (before the
    * linker drops varyings the other stage doesn't use) */
   struct sgx_varyings vary[MESA_SHADER_STAGES];
   memset(vary, 0, sizeof(vary));
   for (unsigned i = 0; i < prog->NumShaders; i++) {
      struct gl_shader *sh = prog->Shaders[i];
      if (sh->nir && (sh->Stage == MESA_SHADER_VERTEX || sh->Stage == MESA_SHADER_FRAGMENT))
         sgx_assign_varyings(sh->nir, sh->Stage == MESA_SHADER_VERTEX, &vary[sh->Stage]);
   for (unsigned st = MESA_SHADER_VERTEX; st <= MESA_SHADER_FRAGMENT && opts.verbose; st++)
      for (unsigned i = 0; i < vary[st].n; i++)
         fprintf(stderr, "%s varying %s: set %u, %u x %u\n", st == MESA_SHADER_FRAGMENT ? "frag" : "vert", vary[st].v[i].name,
                 vary[st].v[i].set, vary[st].v[i].rows, vary[st].v[i].comps);
   }

   /* Link each stage on its own, as a separable program: the DDK compiles
    * (and lays out) each shader independently, so Mesa's cross-stage varying
    * optimisations (dropping or compacting components the other stage
    * doesn't read) must not happen. */
   int status = 0;
   for (unsigned i = 0; i < prog->NumShaders; i++) {
      struct gl_shader *sh = prog->Shaders[i];
      unsigned st = sh->Stage;
      if (!sh->CompileStatus) {
         fprintf(stderr, "sgxc: compile failed: %s\n", sh->InfoLog);
         status = 1;
         continue;
      }
      if (st != MESA_SHADER_VERTEX && st != MESA_SHADER_FRAGMENT)
         continue;
      struct gl_shader_program *sep = standalone_create_shader_program();
      sep->SeparateShader = true;
      sep->Shaders = (struct gl_shader **)ralloc_array(sep, struct gl_shader *, 1);
      sep->Shaders[0] = sh;
      sep->NumShaders = 1;
      _mesa_clear_shader_program_data(ctx, sep);
      sep->data->LinkStatus = LINKING_SUCCESS;
      link_shaders_init(ctx, sep);
      gl_nir_link_glsl(ctx, sep);
      if (!sep->data->LinkStatus || !sep->_LinkedShaders[st]) {
         fprintf(stderr, "sgxc: link failed: %s\n", sep->data->InfoLog);
         status = 1;
         continue;
      }
      nir_shader *nir = sep->_LinkedShaders[st]->Program->nir;
      /* the standalone frontend builds NIR with its own options */
      nir->options = sgx_nir_options();
      const char *sname = st == MESA_SHADER_VERTEX ? "vert" : "frag";
      /* this stage's pins: unprefixed ones, and "vert:" / "frag:" ones */
      struct sgx_compile_options so2 = opts;
      static struct pins mine;
      memset(&mine, 0, sizeof(mine));
      for (unsigned ti = 0; ti < 3; ti++) {
         for (unsigned i = 0; i < pins.n[ti]; i++) {
            struct named e = pins.t[ti][i];
            const char *c = strchr(e.name, ':');
            if (c) {
               if (strncmp(e.name, sname, c - e.name) || strlen(sname) != (size_t)(c - e.name))
                  continue;
               memmove(e.name, c + 1, strlen(c + 1) + 1);
            }
            mine.t[ti][mine.n[ti]++] = e;
         }
      }
      struct sgx_layout layout = { pins_place, pins_first_free, &mine };
      so2.layout = &layout;
      if (driver_uniforms) {
         /* -L: uniforms lowered before the compiler sees them, the way the
          * gallium driver hands them over (st's vec4 driver_location, then
          * the driver's rescale to components); constant numbers are then
          * the driver's, not glslcompiler's */
         unsigned loc = 0;
         nir_foreach_variable_with_modes(var, nir, nir_var_uniform) {
            if (glsl_type_is_sampler(glsl_without_array(var->type)))
               continue;
            var->data.driver_location = loc;
            loc += glsl_count_vec4_slots(var->type, false, false);
         }
         NIR_PASS(_, nir, nir_lower_io, nir_var_uniform, vec4_slots, (nir_lower_io_options)0);
         /* (st hands over constant offsets folded: a struct member's is an
          * iadd of constants until then) */
         NIR_PASS(_, nir, nir_opt_constant_folding);
         NIR_PASS(_, nir, nir_shader_intrinsics_pass, rescale_uniform, nir_metadata_control_flow, NULL);
      }
      /* (a textureSize, its own or one this adds, is the driver's to lower:
       * sgxc refuses it) */
      if (so2.native_integers)
         sgx_lower_tex(nir);
      struct sgx_shader out;
      memset(&out, 0, sizeof(out));
      int r = sgx_compile(nir, &so2, &vary[st], &out);
      char suffix[32];
      if (out.log) {
         snprintf(suffix, sizeof suffix, "%s.txt", sname);
         write_file(prefix, suffix, out.log, strlen(out.log));
         if (opts.verbose || r)
            fputs(out.log, stderr);
      }
      if (r) {
         fprintf(stderr, "sgxc: %s shader: compile failed\n", sname);
         status = 1;
         continue;
      }
      uint8_t *usp;
      size_t usp_size;
      if (out.mem_dwords) {
         /* the USP has no list B writer: code only */
         fprintf(stderr, "sgxc: %s shader: %u constants, %u SA slots, %u words in memory: no USP\n", sname,
                 out.n_consts, out.sa_slots, out.mem_dwords);
      } else if (out.writes_depth) {
         fprintf(stderr, "sgxc: %s shader: gl_FragDepth in pa%u: no USP\n", sname, out.depth_pa);
      } else if (sgx_write_usp(&out, &usp, &usp_size)) {
         fprintf(stderr, "sgxc: %s shader: USP: out of memory\n", sname);
         status = 1;
         continue;
      } else {
         snprintf(suffix, sizeof suffix, "%s.usp", sname);
         write_file(prefix, suffix, usp, usp_size);
         free(usp);
      }
      char *hex = (char *)malloc(out.n_code * 20 + 1);
      size_t k = 0;
      for (unsigned i = 0; i < out.n_code; i++)
         k += snprintf(hex + k, 20, "%08x %08x\n", (unsigned)(out.code[i] >> 32), (unsigned)out.code[i]);
      snprintf(suffix, sizeof suffix, "%s.code", sname);
      write_file(prefix, suffix, hex, k);
      free(hex);
      if (out.n_code1) {
         /* -P: phase 1 of a split program */
         hex = (char *)malloc(out.n_code1 * 20 + 1);
         k = 0;
         for (unsigned i = 0; i < out.n_code1; i++)
            k += snprintf(hex + k, 20, "%08x %08x\n", (unsigned)(out.code1[i] >> 32), (unsigned)out.code1[i]);
         snprintf(suffix, sizeof suffix, "%s.code1", sname);
         write_file(prefix, suffix, hex, k);
         free(hex);
         printf("%s phase 1: %u instructions\n", sname, out.n_code1);
      }
      printf("%s: %u instructions (%u alu, %u tex, %u flow), %u temps, %u pa\n", sname,
             out.n_code, out.n_alu, out.n_tex, out.n_flow, out.n_temps, out.n_pa);
      sgx_shader_free(&out);
   }
   standalone_compiler_cleanup(prog, ctx);
   return status;
}
