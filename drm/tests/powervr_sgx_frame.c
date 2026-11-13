// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * A real frame through powervr_sgx: tools/gen's frame builder (sgxgen, built
 * with -DSGXGEN_DRM) records a clear and a triangle, and one SUBMIT_JOBS render
 * job draws it. The pixels are checked against what the frame should contain.
 *
 *   powervr_sgx_frame [-v] [-k kicks] [-n frames] [-q depth] [-s WxH] [-c contexts] [-C]
 *                     [-o out.bgra]
 *
 * -k 2 (default) records the scene as two TA kicks, the stream and then the
 * terminate words; -k 1 as one kick that also renders. -r gives every frame a
 * new render target (RT dataset). -c N draws the frames round robin on N render
 * contexts in the same VM; -C replaces the context after every frame (same VM).
 * -q N submits each frame's render N times in one SUBMIT_JOBS, so N jobs queue
 * behind each other on the same RT dataset and run back to back.
 * -D prints debugfs "render_before" (with powervr_sgx.snapshot=1, the
 * structures as the last kick was written) and "render" (what the microkernel
 * left in them) after every frame; SGXGEN_DUMP_PROGS=1 adds the client's 3D
 * programs.
 *
 * Exit 0 when every frame's pixels are right.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "sgxgen.h"

static void dump_file(const char *name, const char *title, unsigned f)
{
	char path[128], line[512];
	FILE *fp;

	snprintf(path, sizeof path, "/sys/kernel/debug/dri/128/%s", name);
	fp = fopen(path, "r");
	if (!fp)
		return;
	printf("== %s frame %u\n", title, f + 1);
	while (fgets(line, sizeof line, fp))
		fputs(line, stdout);
	fclose(fp);
}

static void dump_render(unsigned f)
{
	usleep(100000);
	dump_file("render_before", "before the 3D of", f);
	dump_file("render", "after", f);
}

/* The mapped RT structures after a render, read straight from the client's BOs
 * (works with no render debugfs). Fields and format match tools/gen dump_after /
 * dump64.txt, so a 530 result diffs 1:1 against the 64x64 reference. rgn0/1 and
 * tailptr are the "did the TA emit tiles" signal. */
static void dump_bo(const char *name, const struct sgx_bo *b, uint32_t words)
{
	/* Kernel-owned structures (e.g. region headers) are not client-mapped. */
	if (!b->map) {
		printf("%s @ 0x%08x: <not client-mapped>\n", name, b->va);
		return;
	}
	printf("%s @ 0x%08x:", name, b->va);
	for (uint32_t i = 0; i < words; i++)
		printf("%s%08x", i % 8 ? " " : "\n  ", b->map[i]);
	putchar('\n');
}

static void dump_rt(const struct sgx_surface *s)
{
	printf("== RT structures after render\n");
	dump_bo("rtdata", &s->rtdata, (2 * 0x38 + 0x18) / 4);
	dump_bo("rtword", &s->rtword, 1);
	dump_bo("entword0", &s->entword[0], 1);
	dump_bo("entword1", &s->entword[1], 1);
	dump_bo("rgn0", &s->rgn[0], 0x60 / 4);
	dump_bo("rgn1", &s->rgn[1], 0x60 / 4);
	dump_bo("tailptr", &s->tailptr, 0x40 / 4);
	dump_bo("hwsync", &s->hwsync, 0x28 / 4);
}

/* The frame's render, depth times back to back in one SUBMIT_JOBS, then a wait. */
static int flush_deep(struct sgx_ctx *c, unsigned depth, unsigned timeout_ms)
{
	static struct sgx_job js[64];
	static struct drm_powervr_sgx_job jj[64];
	struct drm_powervr_sgx_ioctl_submit_jobs_args sj = {
		.jobs = { .stride = sizeof(jj[0]), .count = depth, .array = (uintptr_t)jj },
	};
	uint32_t h = c->syncobj;
	struct drm_syncobj_wait w = { .handles = (uintptr_t)&h, .count_handles = 1 };
	struct timespec ts;
	int e;

	for (unsigned q = 0; q < depth; q++) {
		sgx_render_job(c, &js[q]);
		jj[q] = js[q].job;
	}
	if (ioctl(c->dev->fd, DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &sj)) {
		e = -errno;
		fprintf(stderr, "powervr_sgx_frame: SUBMIT_JOBS: %d (job %d)\n", e, (int)sj.failed_index);
		return e;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	w.timeout_nsec = ts.tv_sec * 1000000000ll + ts.tv_nsec +
			 (int64_t)timeout_ms * depth * 1000000;
	return ioctl(c->dev->fd, DRM_IOCTL_SYNCOBJ_WAIT, &w) ? -errno : 0;
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
	unsigned depth = 1;

	while ((opt = getopt(argc, argv, "vDrCc:k:n:q:s:o:")) != -1) {
		switch (opt) {
		case 'v': verbose = 1; break;
		case 'k': kicks = atoi(optarg); break;
		case 'r': fresh = 1; break;	/* a new RT dataset every frame */
		case 'D': dump = 1; break;
		case 'c': nctx = atoi(optarg); break;
		case 'C': renew = 1; break;	/* a new context every frame */
		case 'q': depth = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'n': frames = (unsigned)strtoul(optarg, NULL, 0); break;
		case 's':
			if (sscanf(optarg, "%ux%u", &w, &h) != 2)
				return 2;
			break;
		case 'o': out = optarg; break;
		default:
			fprintf(stderr, "usage: powervr_sgx_frame [-v] [-D] [-r] [-C] [-c contexts] [-k kicks] [-n frames] [-q depth] [-s WxH] [-o out]\n");
			return 2;
		}
	}
	if ((e = sgx_dev_open(&dev, NULL, NULL))) {
		fprintf(stderr, "powervr_sgx_frame: open: %d\n", e);
		return 77;
	}
	dev.verbose = verbose;
	if (nctx < 1 || nctx > 8 || depth < 1 || depth > 64)
		return 2;
	for (int i = 0; i < nctx; i++) {
		cs[i] = sgx_ctx_create(&dev);
		cs[i]->ta_kicks = kicks;
		ss[i] = sgx_surface_create(cs[i], w, h, 0);
		vss[i] = sgx_shader_builtin(cs[i], SGX_VS_XY);
		fss[i] = sgx_shader_builtin(cs[i], SGX_FS_UNIFORM);
		sgx_ctx_mark(cs[i]);
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
			sgx_ctx_mark(cs[i]);
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
		e = depth > 1 ? flush_deep(c, depth, 2000) : sgx_flush(c, 2000);
		centre = px(s, w / 2, h / 2);
		corner = px(s, 2, 2);
		printf("frame %u: render %s, centre %08x (want %08x), corner %08x (want %08x)\n", f,
		       e ? "FAILED" : "done", centre, want_tri, corner, want_bg);
		if (e || centre != want_tri || corner != want_bg)
			bad++;
		if (dump)
			dump_rt(s);
		/* The frame is done (or reset): its programs and data can go. */
		sgx_ctx_rewind(c);
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
