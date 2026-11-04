// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * PRIME between powervr_sgx and a KMS device (simpledrm on the EA), end to end:
 * frames rendered by powervr_sgx are scanned out by the KMS device.
 *
 *   powervr_sgx_prime [-v] [-n frames] [-d ms] [-a | -b]
 *
 * a) the KMS device's dumb buffer, exported and imported into powervr_sgx
 *    (Mesa's kmsro path): the GPU renders into it, it is the scanout fb.
 * b) a powervr_sgx object, exported and imported into the KMS device as the fb.
 *
 * Every frame's pixels are checked in the buffer the GPU wrote and, through
 * /dev/mem, in the framebuffer the display scans out (simpledrm copies to it
 * on DIRTYFB). Before each frame the buffer is filled from the CPU, so stale
 * cache lines would show. Frames take turns:
 *
 *   explicit  wait for the job's syncobj, then DIRTYFB;
 *   implicit  the buffer in the job's BO list as written, DIRTYFB at once: the
 *             KMS commit must wait for the job's fence in the shared dma_resv;
 *   control   no BO list, DIRTYFB at once: the scanout may well be stale, which
 *             shows the implicit frames' check can fail (not counted).
 *
 * -d holds each frame on the screen. Exit 0 when every counted frame is right
 * in both places.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include "sgxgen.h"

/* The CEFDK framebuffer simpledrm drives (board DT: framebuffer@7fc00000). */
#define SCANOUT_PA	0x7fc00000u
#define POISON		0xdeadbeefu

static int verbose;

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r ? -errno : 0;
}

#define CHECK(call) do { int _e = (call); if (_e) { \
	fprintf(stderr, "powervr_sgx_prime: %s: %s\n", #call, strerror(-_e)); exit(1); } } while (0)

struct kms {
	int fd;
	uint32_t crtc, conn;
	struct drm_mode_modeinfo mode;
	uint32_t *scanout;		/* the display's framebuffer, or NULL */
	uint32_t scanout_stride;	/* pixels */
};

static int open_kms(struct kms *k, const char *want)
{
	for (int i = 0; i < 8; i++) {
		char path[32], name[64] = "";
		struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };
		int fd;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!xioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, want)) {
			k->fd = fd;
			if (verbose)
				fprintf(stderr, "kms: %s is %s\n", path, name);
			return 0;
		}
		close(fd);
	}
	return -ENODEV;
}

static void kms_setup(struct kms *k)
{
	struct drm_mode_card_res res = {};
	struct drm_mode_get_connector gc = {};
	uint32_t crtcs[8], conns[8];
	struct drm_mode_modeinfo modes[16];

	CHECK(xioctl(k->fd, DRM_IOCTL_MODE_GETRESOURCES, &res));
	if (!res.count_crtcs || !res.count_connectors || res.count_crtcs > 8 || res.count_connectors > 8) {
		fprintf(stderr, "powervr_sgx_prime: %u crtcs, %u connectors\n", res.count_crtcs,
			res.count_connectors);
		exit(1);
	}
	res = (struct drm_mode_card_res){
		.crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = res.count_crtcs,
		.connector_id_ptr = (uintptr_t)conns, .count_connectors = res.count_connectors,
	};
	CHECK(xioctl(k->fd, DRM_IOCTL_MODE_GETRESOURCES, &res));
	k->crtc = crtcs[0];
	k->conn = conns[0];

	gc.connector_id = k->conn;
	CHECK(xioctl(k->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc));
	if (!gc.count_modes || gc.count_modes > 16) {
		fprintf(stderr, "powervr_sgx_prime: connector has %u modes\n", gc.count_modes);
		exit(1);
	}
	gc = (struct drm_mode_get_connector){
		.connector_id = k->conn, .modes_ptr = (uintptr_t)modes, .count_modes = gc.count_modes,
	};
	CHECK(xioctl(k->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc));
	k->mode = modes[0];
	printf("kms: crtc %u connector %u mode %ux%u\n", k->crtc, k->conn, k->mode.hdisplay,
	       k->mode.vdisplay);
}

static void scanout_map(struct kms *k)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	size_t len = (size_t)k->scanout_stride * 4 * k->mode.vdisplay;
	void *p;

	if (fd < 0) {
		printf("scanout: /dev/mem: %s, not checked\n", strerror(errno));
		return;
	}
	p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, SCANOUT_PA);
	close(fd);
	if (p == MAP_FAILED) {
		printf("scanout: mmap of %#x: %s, not checked\n", SCANOUT_PA, strerror(errno));
		return;
	}
	k->scanout = p;
}

static uint32_t kms_addfb(struct kms *k, uint32_t handle, uint32_t pitch)
{
	struct drm_mode_fb_cmd2 fb = {
		.width = k->mode.hdisplay, .height = k->mode.vdisplay,
		.pixel_format = DRM_FORMAT_XRGB8888,
		.handles = { handle }, .pitches = { pitch },
	};

	CHECK(xioctl(k->fd, DRM_IOCTL_MODE_ADDFB2, &fb));
	return fb.fb_id;
}

static void kms_show(struct kms *k, uint32_t fb_id)
{
	struct drm_mode_crtc sc = {
		.crtc_id = k->crtc, .fb_id = fb_id,
		.set_connectors_ptr = (uintptr_t)&k->conn, .count_connectors = 1,
		.mode = k->mode, .mode_valid = 1,
	};

	CHECK(xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &sc));
}

static void kms_dirty(struct kms *k, uint32_t fb_id)
{
	struct drm_mode_fb_dirty_cmd d = { .fb_id = fb_id };

	CHECK(xioctl(k->fd, DRM_IOCTL_MODE_DIRTYFB, &d));
}

static int prime_move(int from, uint32_t handle, int to, uint32_t *out)
{
	struct drm_prime_handle ex = { .handle = handle, .flags = DRM_CLOEXEC | DRM_RDWR };
	struct drm_prime_handle im = {};
	int e;

	if ((e = xioctl(from, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ex)))
		return e;
	im.fd = ex.fd;
	e = xioctl(to, DRM_IOCTL_PRIME_FD_TO_HANDLE, &im);
	close(ex.fd);
	*out = im.handle;
	return e;
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Submit the frame, the colour buffer in its BO list if bo, and DIRTYFB before (or after) the wait. */
static int submit_frame(struct sgx_ctx *c, struct kms *k, uint32_t fb_id, uint32_t bo, int dirty_first)
{
	struct sgx_job j;
	struct drm_powervr_sgx_bo_ref ref = { .handle = bo, .flags = DRM_POWERVR_SGX_BO_REF_FLAG_WRITE };
	struct drm_powervr_sgx_ioctl_submit_jobs_args sj = {
		.jobs = { .stride = sizeof(j.job), .count = 1, .array = (uintptr_t)&j.job },
	};
	uint32_t h = c->syncobj;
	struct drm_syncobj_wait w = { .handles = (uintptr_t)&h, .count_handles = 1 };
	int e;

	sgx_render_job(c, &j);
	if (bo)
		j.job.bo_refs = (struct drm_powervr_sgx_obj_array){
			.stride = sizeof(ref), .count = 1, .array = (uintptr_t)&ref,
		};
	if ((e = xioctl(c->dev->fd, DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &sj)))
		return e;
	if (dirty_first)
		kms_dirty(k, fb_id);
	w.timeout_nsec = now_ns() + 2000000000ll;
	e = xioctl(c->dev->fd, DRM_IOCTL_SYNCOBJ_WAIT, &w);
	if (!dirty_first)
		kms_dirty(k, fb_id);
	return e;
}

/* Pixels of the w x h image at p (stride in pixels) that are not what the frame should hold. */
static unsigned check(const uint32_t *p, uint32_t stride, unsigned w, unsigned h, uint32_t tri, uint32_t bg,
		      unsigned *poison)
{
	unsigned bad = 0;

	*poison = 0;
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) {
			uint32_t v = p[(size_t)y * stride + x];

			if (v == POISON)
				(*poison)++;
			else if (v != tri && v != bg)
				bad++;
		}
	/* the triangle's centre and a corner */
	if (p[(size_t)(h / 2) * stride + w / 2] != tri || p[2 * stride + 2] != bg)
		bad++;
	return bad + *poison;
}

int main(int argc, char **argv)
{
	struct sgx_state st = { .front = SGX_CCW };
	const float clear[4] = { 0, 0, 0.5f, 1 };
	const float tri[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f };
	unsigned frames = 6, hold_ms = 0, bad_frames = 0;
	int opt, e, path = 0;	/* 0: both, 1: a, 2: b */
	struct kms k = {};
	struct sgx_dev dev;

	while ((opt = getopt(argc, argv, "vn:d:ab")) != -1) {
		switch (opt) {
		case 'v': verbose = 1; break;
		case 'n': frames = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'd': hold_ms = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'a': path = 1; break;
		case 'b': path = 2; break;
		default:
			fprintf(stderr, "usage: powervr_sgx_prime [-v] [-n frames] [-d ms] [-a | -b]\n");
			return 2;
		}
	}
	if (open_kms(&k, "simpledrm")) {
		fprintf(stderr, "powervr_sgx_prime: no simpledrm device\n");
		return 77;
	}
	if ((e = sgx_dev_open(&dev, NULL, NULL))) {
		fprintf(stderr, "powervr_sgx_prime: open: %d\n", e);
		return 77;
	}
	dev.verbose = verbose;
	kms_setup(&k);
	/* the GPU's stride is a multiple of 32 pixels; the CEFDK buffer's is 736 for 720 */
	k.scanout_stride = (k.mode.hdisplay + 31) & ~31u;
	scanout_map(&k);

	for (int pass = 1; pass <= 2; pass++) {
		unsigned w = k.mode.hdisplay, h = k.mode.vdisplay, stride = (w + 31) & ~31u;
		uint32_t pitch = stride * 4, size = (pitch * h + 0xfff) & ~0xfffu;
		uint32_t kms_handle, sgx_handle, fb_id;
		struct sgx_ctx *c;
		struct sgx_surface *s;
		struct sgx_shader *vs, *fs;
		uint32_t *map;

		if (path && path != pass)
			continue;
		if (pass == 1) {
			/* a) the KMS device's dumb buffer into powervr_sgx */
			struct drm_mode_create_dumb cd = { .width = stride, .height = h, .bpp = 32 };
			struct drm_mode_map_dumb md = {};

			CHECK(xioctl(k.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd));
			if (cd.pitch != pitch) {
				fprintf(stderr, "powervr_sgx_prime: dumb pitch %u, want %u\n", cd.pitch, pitch);
				return 1;
			}
			kms_handle = cd.handle;
			size = cd.size;
			md.handle = kms_handle;
			CHECK(xioctl(k.fd, DRM_IOCTL_MODE_MAP_DUMB, &md));
			map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, k.fd, md.offset);
			if (map == MAP_FAILED)
				return 1;
			CHECK(prime_move(k.fd, kms_handle, dev.fd, &sgx_handle));
			printf("a) simpledrm dumb buffer %u (%ux%u, pitch %u) -> powervr_sgx handle %u\n",
			       kms_handle, stride, h, cd.pitch, sgx_handle);
		} else {
			/* b) a powervr_sgx object into the KMS device */
			struct drm_powervr_sgx_ioctl_create_bo_args cb = {
				.size = size, .flags = DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS,
			};
			struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args mo = {};

			CHECK(xioctl(dev.fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &cb));
			sgx_handle = cb.handle;
			mo.handle = sgx_handle;
			CHECK(xioctl(dev.fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &mo));
			map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd, mo.offset);
			if (map == MAP_FAILED)
				return 1;
			CHECK(prime_move(dev.fd, sgx_handle, k.fd, &kms_handle));
			printf("b) powervr_sgx object %u (%u bytes) -> simpledrm handle %u\n", sgx_handle, size,
			       kms_handle);
		}

		c = sgx_ctx_create(&dev);
		s = sgx_surface_import(c, w, h, stride, sgx_handle, size, map);
		vs = sgx_shader_builtin(c, SGX_VS_XY);
		fs = sgx_shader_builtin(c, SGX_FS_UNIFORM);
		fb_id = kms_addfb(&k, kms_handle, pitch);
		for (unsigned i = 0; i < (size_t)stride * h; i++)
			map[i] = POISON;
		kms_show(&k, fb_id);

		for (unsigned f = 0; f < frames; f++) {
			struct sgx_draw d = {
				.prim = SGX_TRIANGLES, .count = 3, .verts = tri, .state = &st,
				.vs = vs, .fs = fs, .color = { f & 1 ? 1.0f : 0, 1, 0, 1 },
			};
			uint32_t want_tri = f & 1 ? 0xffffff00 : 0xff00ff00, want_bg = 0xff000080;
			unsigned gpu_bad, gpu_poison, out_bad = 0, out_poison = 0;
			static const char *const mode_name[] = { "explicit", "implicit", "control" };
			int mode = f % 3;

			for (unsigned i = 0; i < (size_t)stride * h; i++)
				map[i] = POISON;
			sgx_set_framebuffer(c, s);
			sgx_clear(c, clear, 1.0f);
			if ((e = sgx_draw(c, &d)) ||
			    (e = submit_frame(c, &k, fb_id, mode == 1 ? sgx_handle : 0, mode != 0))) {
				printf("%c) frame %u: render failed: %d\n", 'a' + pass - 1, f, e);
				bad_frames++;
				continue;
			}
			gpu_bad = check(map, stride, w, h, want_tri, want_bg, &gpu_poison);
			if (k.scanout) {
				/* the display ignores alpha: compare XRGB */
				out_bad = 0;
				for (unsigned y = 0; y < h; y++)
					for (unsigned x = 0; x < w; x++) {
						uint32_t v = k.scanout[(size_t)y * k.scanout_stride + x] | 0xff000000u;

						if (v == (POISON | 0xff000000u))
							out_poison++;
						else if (v != want_tri && v != want_bg)
							out_bad++;
					}
				if ((k.scanout[(size_t)(h / 2) * k.scanout_stride + w / 2] | 0xff000000u) != want_tri)
					out_bad++;
			}
			printf("%c) frame %u %-8s: buffer %s (centre %08x, %u wrong, %u stale), scanout %s (%u wrong, %u stale)\n",
			       'a' + pass - 1, f, mode_name[mode], gpu_bad ? "WRONG" : "right",
			       map[(size_t)(h / 2) * stride + w / 2], gpu_bad, gpu_poison,
			       !k.scanout ? "not checked" : out_bad + out_poison ? "WRONG" : "right",
			       out_bad, out_poison);
			if (gpu_bad || (mode != 2 && (out_bad || out_poison)))
				bad_frames++;
			if (hold_ms)
				usleep(hold_ms * 1000);
		}
		sgx_surface_destroy(c, s);
		sgx_ctx_destroy(c);
		{
			uint32_t id = fb_id;

			xioctl(k.fd, DRM_IOCTL_MODE_RMFB, &id);
		}
		munmap(map, size);
	}
	sgx_dev_close(&dev);
	printf("%s\n", bad_frames ? "SOME FRAMES WRONG" : "all frames right");
	return bad_frames ? 1 : 0;
}
