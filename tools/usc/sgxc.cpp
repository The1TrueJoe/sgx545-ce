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

static void
named(struct sgx_named *tab, unsigned *n, const char *arg)
{
   const char *eq = strchr(arg, '=');
   if (!eq || *n >= SGX_MAX_NAMED || eq - arg >= 63) {
      fprintf(stderr, "sgxc: bad name=value '%s'\n", arg);
      exit(2);
   }
   memcpy(tab[*n].name, arg, eq - arg);
   tab[*n].name[eq - arg] = 0;
   tab[*n].value = atoi(eq + 1);
   (*n)++;
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

/* the gallium driver's (sgx545_program.c): load_uniform base and offset from
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
   const char *prefix = "out";
   int c;

   memset(&opts, 0, sizeof(opts));
   bool lowp16 = false, driver_uniforms = false;
   while ((c = getopt(argc, argv, "vpFLDu:a:s:o:")) != -1) {
      switch (c) {
      case 'F': opts.front_p0 = true; break;
      case 'L': driver_uniforms = true; break;
      case 'D': opts.driver_consts = true; break;   /* constant formats only our driver loads */
      case 'p': lowp16 = true; opts.f16 = true; break;
      case 'v': opts.verbose = true; break;
      case 'u': named(opts.uniforms, &opts.n_uniforms, optarg); break;
      case 'a': named(opts.attribs, &opts.n_attribs, optarg); break;
      case 's': named(opts.samplers, &opts.n_samplers, optarg); break;
      case 'o': prefix = optarg; break;
      default:
         fprintf(stderr, "usage: sgxc [-v] [-p] [-L] [-D] [-u name=k] [-a name=slot] [-s name=unit] -o prefix a.vert b.frag\n");
         return 2;
      }
   }

   struct standalone_options so;
   memset(&so, 0, sizeof(so));
   so.glsl_version = 100;
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
      const char *sname = st == MESA_SHADER_VERTEX ? "vert" : "frag";
      /* this stage's pins: unprefixed ones, and "vert:" / "frag:" ones */
      struct sgx_compile_options so2 = opts;
      struct { struct sgx_named *t; unsigned *n; } tabs[] = {
         { so2.uniforms, &so2.n_uniforms }, { so2.attribs, &so2.n_attribs }, { so2.samplers, &so2.n_samplers } };
      for (unsigned ti = 0; ti < 3; ti++) {
         unsigned k = 0;
         for (unsigned i = 0; i < *tabs[ti].n; i++) {
            struct sgx_named e = tabs[ti].t[i];
            const char *c = strchr(e.name, ':');
            if (c) {
               bool mine = !strncmp(e.name, sname, c - e.name) && strlen(sname) == (size_t)(c - e.name);
               if (!mine)
                  continue;
               memmove(e.name, c + 1, strlen(c + 1) + 1);
            }
            tabs[ti].t[k++] = e;
         }
         *tabs[ti].n = k;
      }
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
         NIR_PASS(_, nir, nir_shader_intrinsics_pass, rescale_uniform, nir_metadata_control_flow, NULL);
      }
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
      snprintf(suffix, sizeof suffix, "%s.usp", sname);
      write_file(prefix, suffix, out.usp, out.usp_size);
      char *hex = (char *)malloc(out.n_code * 20 + 1);
      size_t k = 0;
      for (unsigned i = 0; i < out.n_code; i++)
         k += sprintf(hex + k, "%08x %08x\n", (unsigned)(out.code[i] >> 32), (unsigned)out.code[i]);
      snprintf(suffix, sizeof suffix, "%s.code", sname);
      write_file(prefix, suffix, hex, k);
      free(hex);
      printf("%s: %u instructions (%u alu, %u tex, %u flow), %u temps, %u pa\n", sname,
             out.n_code, out.n_alu, out.n_tex, out.n_flow, out.n_temps, out.n_pa);
      sgx_shader_free(&out);
   }
   standalone_compiler_cleanup(prog, ctx);
   return status;
}
