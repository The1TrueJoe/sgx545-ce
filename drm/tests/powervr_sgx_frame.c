// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * A real frame through powervr_sgx: tools/gen's frame builder (sgxgen, built
 * with -DSGXGEN_DRM) records a clear and a triangle, and one SUBMIT_JOBS render
 * job draws it. The pixels are checked against what the frame should contain.
 *
 *   powervr_sgx_frame [-v] [-n frames] [-s WxH] [-o out.bgra]
 *
 * Exit 0 when every frame's pixels are right.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sgxgen.h"

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
	struct sgx_shader *vs, *fs;
	struct sgx_surface *s;
	struct sgx_ctx *c;
	struct sgx_dev dev;
	int opt, e, verbose = 0;

	while ((opt = getopt(argc, argv, "vn:s:o:")) != -1) {
		switch (opt) {
		case 'v': verbose = 1; break;
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
	c = sgx_ctx_create(&dev);
	s = sgx_surface_create(c, w, h, 0);
	vs = sgx_shader_builtin(c, SGX_VS_XY);
	fs = sgx_shader_builtin(c, SGX_FS_UNIFORM);

	for (unsigned f = 0; f < frames; f++) {
		/* each frame a different triangle colour, so a stale frame cannot pass */
		struct sgx_draw d = {
			.prim = SGX_TRIANGLES, .count = 3, .verts = tri, .vs = vs, .fs = fs, .state = &st,
			.color = { f & 1 ? 1.0f : 0, 1, 0, 1 },
		};
		uint32_t want_tri = f & 1 ? 0xffffff00 : 0xff00ff00, want_bg = 0xff000080;
		uint32_t centre, corner;

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
	}

	if (out) {
		FILE *fp = fopen(out, "wb");

		if (fp) {
			fwrite(s->color.map, 4, (size_t)s->stride * h, fp);
			fclose(fp);
		}
	}
	sgx_surface_destroy(c, s);
	sgx_ctx_destroy(c);
	sgx_dev_close(&dev);
	printf("%u/%u frames right\n", frames - bad, frames);
	return bad ? 1 : 0;
}
