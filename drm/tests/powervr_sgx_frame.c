// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * A real frame through powervr_sgx: tools/gen's frame builder (sgxgen, built
 * with -DSGXGEN_DRM) records a clear and a triangle, and one SUBMIT_JOBS render
 * job draws it. The pixels are checked against what the frame should contain.
 *
 *   powervr_sgx_frame [-v] [-k kicks] [-n frames] [-s WxH] [-c contexts] [-C] [-o out.bgra]
 *
 * -k 2 (default) records the scene as two TA kicks, the stream and then the
 * terminate words; -k 1 as one kick that also renders. -r gives every frame a
 * new render target (RT dataset). -c N draws the frames round robin on N render
 * contexts in the same VM; -C replaces the context after every frame (same VM). -D prints debugfs "render" after every frame:
 * what the microkernel left in the kernel-built structures.
 *
 * Exit 0 when every frame's pixels are right.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sgxgen.h"

static void dump_render(unsigned f)
{
	FILE *fp;
	char line[512];

	usleep(100000);
	fp = fopen("/sys/kernel/debug/dri/0000:01:02.0/render", "r");
	if (!fp)
		return;
	printf("== after frame %u\n", f + 1);
	while (fgets(line, sizeof line, fp))
		fputs(line, stdout);
	fclose(fp);
}

static uint32_t px(const struct sgx_surface *s, unsigned x, unsigned y)
{
	return s->color.map[y * s->stride + x];
}

int main(int argc, char **argv)
{
	struct sgx_state st = { .front = SGX_CCW };
	const float clear[4] = { 0, 0, 0.5f, 1 };
	const float tri[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f };
	unsigned w = 256, h = 256, frames = 1, bad = 0;
	const char *out = NULL;
	struct sgx_shader *vss[8], *fss[8];
	struct sgx_surface *ss[8], *s;
	struct sgx_ctx *cs[8], *c;
	struct sgx_dev dev;
	int opt, e, verbose = 0, kicks = 2, fresh = 0, dump = 0, nctx = 1, renew = 0;

	while ((opt = getopt(argc, argv, "vDrCc:k:n:s:o:")) != -1) {
		switch (opt) {
		case 'v': verbose = 1; break;
		case 'k': kicks = atoi(optarg); break;
		case 'r': fresh = 1; break;	/* a new RT dataset every frame */
		case 'D': dump = 1; break;
		case 'c': nctx = atoi(optarg); break;
		case 'C': renew = 1; break;	/* a new context every frame */
		case 'n': frames = (unsigned)strtoul(optarg, NULL, 0); break;
		case 's':
			if (sscanf(optarg, "%ux%u", &w, &h) != 2)
				return 2;
			break;
		case 'o': out = optarg; break;
		default:
			fprintf(stderr, "usage: powervr_sgx_frame [-v] [-n frames] [-s WxH] [-o out]\n");
			return 2;
		}
	}
	if ((e = sgx_dev_open(&dev, NULL, NULL))) {
		fprintf(stderr, "powervr_sgx_frame: open: %d\n", e);
		return 77;
	}
	dev.verbose = verbose;
	if (nctx < 1 || nctx > 8)
		return 2;
	for (int i = 0; i < nctx; i++) {
		cs[i] = sgx_ctx_create(&dev);
		cs[i]->ta_kicks = kicks;
		ss[i] = sgx_surface_create(cs[i], w, h, 0);
		vss[i] = sgx_shader_builtin(cs[i], SGX_VS_XY);
		fss[i] = sgx_shader_builtin(cs[i], SGX_FS_UNIFORM);
	}

	for (unsigned f = 0; f < frames; f++) {
		int i = f % nctx;
		/* each frame a different triangle colour, so a stale frame cannot pass */
		struct sgx_draw d = {
			.prim = SGX_TRIANGLES, .count = 3, .verts = tri, .state = &st,
			.color = { f & 1 ? 1.0f : 0, 1, 0, 1 },
		};
		uint32_t want_tri = f & 1 ? 0xffffff00 : 0xff00ff00, want_bg = 0xff000080;
		uint32_t centre, corner;

		if (renew && f) {
			sgx_surface_destroy(cs[i], ss[i]);
			sgx_ctx_destroy(cs[i]);
			cs[i] = sgx_ctx_create(&dev);
			cs[i]->ta_kicks = kicks;
			ss[i] = sgx_surface_create(cs[i], w, h, 0);
			vss[i] = sgx_shader_builtin(cs[i], SGX_VS_XY);
			fss[i] = sgx_shader_builtin(cs[i], SGX_FS_UNIFORM);
		} else if (fresh && f) {
			sgx_surface_destroy(cs[i], ss[i]);
			ss[i] = sgx_surface_create(cs[i], w, h, 0);
		}
		c = cs[i];
		s = ss[i];
		d.vs = vss[i];
		d.fs = fss[i];
		memset(s->color.map, 0, s->stride * h * 4);
		sgx_set_framebuffer(c, s);
		sgx_clear(c, clear, 1.0f);
		if ((e = sgx_draw(c, &d))) {
			fprintf(stderr, "powervr_sgx_frame: draw: %d\n", e);
			return 1;
		}
		e = sgx_flush(c, 2000);
		centre = px(s, w / 2, h / 2);
		corner = px(s, 2, 2);
		printf("frame %u: render %s, centre %08x (want %08x), corner %08x (want %08x)\n", f,
		       e ? "FAILED" : "done", centre, want_tri, corner, want_bg);
		if (e || centre != want_tri || corner != want_bg)
			bad++;
		if (dump)
			dump_render(f);
	}

	if (out) {
		FILE *fp = fopen(out, "wb");

		if (fp) {
			fwrite(s->color.map, 4, (size_t)s->stride * h, fp);
			fclose(fp);
		}
	}
	for (int i = 0; i < nctx; i++) {
		sgx_surface_destroy(cs[i], ss[i]);
		sgx_ctx_destroy(cs[i]);
	}
	sgx_dev_close(&dev);
	printf("%u/%u frames right\n", frames - bad, frames);
	return bad ? 1 : 0;
}
