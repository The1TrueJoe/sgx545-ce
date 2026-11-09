/* Print the GLES2 shaders gl4es's fixed-pipeline emulator generates for a
 * few Minecraft 1.7.10 states (fpe_VertexShader/FragmentShader + ConvertShader). */
#include <stdio.h>
#include <string.h>
#include "fpe.h"
#include "fpe_shader.h"
#include "shaderconv.h"
#include "init.h"
#include "../glx/hardext.h"

hardext_t hardext;
globals4es_t globals4es;

static void emit(const char *name, fpe_state_t *st)
{
	shaderconv_need_t need;
	memset(&need, 0, sizeof need);
	/* the fixed pipeline's shaders (fpe.c: need NULL); both return the same static buffer */
	char *vs = strdup(fpe_VertexShader(NULL, st)[0]);
	char *fs = strdup(fpe_FragmentShader(NULL, st)[0]);
	char *cvs = strdup(ConvertShader(vs, 1, &need));
	memset(&need, 0, sizeof need);
	char *cfs = strdup(ConvertShader(fs, 0, &need));
	printf("@vert %s\n%s\n@frag %s\n%s\n", name, cvs, name, cfs);
}

int main(void)
{
	hardext.maxtex = 8; hardext.maxlights = 8; hardext.maxplanes = 6; hardext.esversion = 2;
	hardext.maxvattrib = 16; hardext.maxteximage = 8; hardext.maxvarying = 8; hardext.highp = 1;
	hardext.npot = 1; hardext.maxsize = 2048;
	globals4es.vertexfog = 1;
	fpe_state_t st;

	/* terrain: atlas (unit 0) x vertex colour x lightmap (unit 1, texture matrix), alpha > 0.1, linear vertex fog */
	memset(&st, 0, sizeof st);
	st.texture[0].textype = FPE_TEX_2D;
	st.texture[1].textype = FPE_TEX_2D; st.texture[1].texmat = 1;
	st.alphatest = 1; st.alphafunc = FPE_GREATER;
	st.fog = 1; st.fogmode = FPE_FOG_LINEAR; st.fogvertex = 1;
	emit("mc_terrain", &st);

	/* items / entities without the lightmap: texture x colour, alpha test, fog */
	memset(&st, 0, sizeof st);
	st.texture[0].textype = FPE_TEX_2D;
	st.alphatest = 1; st.alphafunc = FPE_GREATER;
	st.fog = 1; st.fogmode = FPE_FOG_LINEAR; st.fogvertex = 1;
	emit("mc_texfog", &st);

	/* GUI / text: texture x colour, alpha test, no fog */
	memset(&st, 0, sizeof st);
	st.texture[0].textype = FPE_TEX_2D;
	st.alphatest = 1; st.alphafunc = FPE_GREATER;
	emit("mc_gui", &st);

	/* sky / untextured: colour with fog */
	memset(&st, 0, sizeof st);
	st.fog = 1; st.fogmode = FPE_FOG_LINEAR; st.fogvertex = 1;
	emit("mc_sky", &st);
	return 0;
}
