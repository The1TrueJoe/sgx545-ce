/*
 * frame: draw one frame on the SGX545 from code alone (tools/gen/sgxgen.c), then
 * dump the render target. Nothing is read from a capture.
 *
 *   frame [-v] [-n node] [-t trace.bin] [-o out.bin] [-s WxH] [-p] [-k r,g,b,a] ops...
 *
 * -p makes the surface y-up, as an EGL pbuffer (rows bottom first in -o).
 *
 * The frame starts with a clear (-k, default 0,0,0.5,1: the probe's), then
 * runs the ops in order:
 *   -d FUNC[,W]          depth test (GL enum, e.g. 0x201 LESS), depth write W (default 1); 0 = off
 *   -V x,y,w,h           glViewport (default: the whole surface)
 *   -C face[,front]      cull (0x404 FRONT, 0x405 BACK; front 0x900 CW, 0x901 CCW)
 *   -T x0,y0,x1,y1,x2,y2[,z]:r,g,b,a
 *                        draw a triangle list (any multiple of 3 vertices) with the
 *                        uniform colour r,g,b,a; with z, a vec3 position at that clip z
 * With no -T it draws the probe's reference triangle
 * (-0.5,-0.5 0.5,-0.5 0,0.5, colour 0,1,0,1).
 *
 * -F ADDR only sends SGX_FLUSH_HW_RENDER_TARGET for another client's RT data set
 * (cmd +0x40 of its TA command), to clean up after a client that didn't.
 *
 * -o writes the colour buffer as the GPU left it: BGRA8888, top row first, pitch
 * w rounded up to 32 (tools/decode/cmpfb.py and tools/decode/fig.py read it). -t writes a
 * pvrtrace log of the run for tools/state/decode.py and tools/pds/dis.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sgxgen.h"

static int floats(const char *s, float *v, int max)
{
	int n = 0;
	char *e;

	while (*s && n < max) {
		v[n++] = strtof(s, &e);
		if (e == s)
			return -1;
		s = *e == ',' ? e + 1 : e;
		if (*e && *e != ',')
			break;
	}
	return n;
}

int main(int argc, char **argv)
{
	struct sgx_dev dev;
	struct sgx_ctx *c;
	struct sgx_surface *s;
	struct sgx_shader *vs_xy, *vs_xyz, *fs;
	struct sgx_state st = { .front = SGX_CCW };
	const char *trace = NULL, *out = NULL, *node = NULL;
	unsigned w = 720, h = 480;
	float clear[4] = { 0, 0, 0.5f, 1 };
	int opt, e, ndraws = 0, verbose = 0;
	struct { char op; char *arg; } ops[64];
	int nops = 0;
	unsigned long flush = 0;
	uint32_t sflags = 0;

	while ((opt = getopt(argc, argv, "vpn:t:o:s:k:d:V:C:T:F:")) != -1) {
		switch (opt) {
		case 'v': verbose = 1; break;
		case 'p': sflags |= SGX_SURFACE_Y_UP; break;
		case 'n': node = optarg; break;
		case 'F': flush = strtoul(optarg, NULL, 0); break;
		case 't': trace = optarg; break;
		case 'o': out = optarg; break;
		case 's':
			if (sscanf(optarg, "%ux%u", &w, &h) != 2)
				return 1;
			break;
		case 'k':
			if (floats(optarg, clear, 4) != 4)
				return 1;
			break;
		case 'd': case 'V': case 'C': case 'T':
			if (nops < 64) {
				ops[nops].op = (char)opt;
				ops[nops++].arg = optarg;
			}
			break;
		default:
			fprintf(stderr, "usage: frame [-v] [-t trace] [-o out] [-s WxH] [-k rgba] [-d f[,w]] "
					"[-V x,y,w,h] [-C face[,front]] [-T verts[,z]:rgba]...\n");
			return 1;
		}
	}
	if ((e = sgx_dev_open(&dev, node, trace))) {
		fprintf(stderr, "frame: services: %d\n", e);
		return 1;
	}
	dev.verbose = verbose;
	if (flush) {
		e = pvr_flush_rt(&dev.pvr, (uint32_t)flush);
		printf("frame: SGX_FLUSH_HW_RENDER_TARGET %#lx: %d\n", flush, e);
		return e ? 1 : 0;
	}
	c = sgx_ctx_create(&dev);
	s = sgx_surface_create(c, w, h, sflags);
	vs_xy = sgx_shader_builtin(c, SGX_VS_XY);
	vs_xyz = sgx_shader_builtin(c, SGX_VS_XYZ);
	fs = sgx_shader_builtin(c, SGX_FS_UNIFORM);
	sgx_set_framebuffer(c, s);
	sgx_clear(c, clear, 1.0f);

	for (int i = 0; i <= nops; i++) {
		char *a = i < nops ? ops[i].arg : NULL;
		float v[64], z[1];

		if (i == nops && ndraws)
			break;
		if (i == nops || ops[i].op == 'T') {
			struct sgx_draw d = { .prim = SGX_TRIANGLES, .vs = vs_xy, .fs = fs, .state = &st,
					      .color = { 0, 1, 0, 1 } };
			float ref[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f }, xyz[96];
			int n = 6, hasz = 0;

			if (a) {
				char *col = strchr(a, ':');

				if (col)
					*col++ = 0;
				n = floats(a, v, 64);
				if (n < 6 || (n % 6 != 0 && n % 6 != 1))
					return fprintf(stderr, "frame: bad -T '%s'\n", a), 1;
				if (n % 6 == 1) {
					z[0] = v[--n];
					hasz = 1;
				}
				if (col && floats(col, d.color, 4) != 4)
					return 1;
			} else {
				memcpy(v, ref, sizeof ref);
			}
			d.count = (uint32_t)n / 2;
			if (hasz) {
				for (uint32_t k = 0; k < d.count; k++) {
					xyz[3 * k] = v[2 * k];
					xyz[3 * k + 1] = v[2 * k + 1];
					xyz[3 * k + 2] = z[0];
				}
				d.vs = vs_xyz;
				d.verts = xyz;
			} else {
				d.verts = v;
			}
			sgx_draw(c, &d);
			ndraws++;
		} else if (ops[i].op == 'd') {
			float f[2] = { 0, 1 };
			int n = floats(a, f, 2);

			st.depth_func = (uint32_t)strtoul(a, NULL, 0);
			st.depth_write = n < 2 || f[1] != 0;
		} else if (ops[i].op == 'V') {
			float f[4];

			if (floats(a, f, 4) != 4)
				return 1;
			st.vp_x = f[0];
			st.vp_y = f[1];
			st.vp_w = f[2];
			st.vp_h = f[3];
		} else if (ops[i].op == 'C') {
			char *x;

			st.cull = (uint32_t)strtoul(a, &x, 0);
			if (*x == ',')
				st.front = (uint32_t)strtoul(x + 1, NULL, 0);
		}
	}
	e = sgx_flush(c, 2000);
	printf("frame: %ux%u, %d draw(s), render %s\n", w, h, ndraws, e ? "TIMED OUT" : "done");
	if (out) {
		FILE *f = fopen(out, "wb");
		size_t n = (size_t)s->stride * h * 4;

		if (!f || fwrite(s->color.map, 1, n, f) != n || fclose(f))
			return perror(out), 1;
	}
	sgx_surface_destroy(c, s);
	sgx_ctx_destroy(c);
	sgx_dev_close(&dev);
	return e ? 2 : 0;
}
