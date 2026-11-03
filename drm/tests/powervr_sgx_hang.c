// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * Mixed submits and hang recovery through powervr_sgx, with sgxgen (frames)
 * and sgxtex (blits) in one VM.
 *
 *   powervr_sgx_hang [-v] [-n rounds] [-H]
 *
 * mixed  one SUBMIT_JOBS: a render on context A, then a transfer that waits on
 *        it and copies A's target to another buffer. The copy must equal the
 *        frame, pixel for pixel.
 * hang   context B draws from an unmapped vertex address (SGXGEN_FAULT): its
 *        render times out and the GPU is reset. B is GUILTY and refused
 *        (-ECANCELED), the reset count went up by one, a devcoredump of
 *        the hang exists, and A (idle at the time), a new context C and the
 *        transfer context still work.
 *
 * idle   (runtime PM) the device autosuspends once idle; a render, a mixed
 *        submit and a blit from suspended each resume it and are right.
 *
 * -H skips the hang case. Exit 0 when everything is right; 77 when there is
 * no powervr_sgx device.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sgxgen.h"
#include "sgxtex.h"

int ioctl(int fd, unsigned long req, ...);

#define W 128
#define H 128

/* sgx_bo_alloc heap ids (sgxgen.c) */
#define HEAP_GENERAL	0
#define HEAP_USSE_PIXEL	4
#define HEAP_PDS_PIXEL	6

#define OBJ_ARRAY(cnt, ptr) { .stride = sizeof((ptr)[0]), .count = (cnt), .array = (__u64)(uintptr_t)(ptr) }

static struct sgx_dev dev;
static int verbose;
static uint32_t xctx, xsync;
static struct sgxtex_blit_ctx X;
static struct sgx_bo usse, bgb, pdsc, pdsp, copy;
static uint32_t emit, eot_next;

static int xioctl(unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(dev.fd, req, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r == -1 ? -errno : 0;
}

static void die(const char *what, int e)
{
	fprintf(stderr, "powervr_sgx_hang: %s: %d\n", what, e);
	exit(1);
}

static int64_t deadline(unsigned ms)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000000000ll + now.tv_nsec + ms * 1000000ll;
}

static int wait_syncobj(uint32_t h, unsigned ms)
{
	struct drm_syncobj_wait w = { .handles = (uintptr_t)&h, .count_handles = 1, .timeout_nsec = deadline(ms) };

	return xioctl(DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

static void put(struct sgx_bo *b, uint32_t va, const uint32_t *w, uint32_t n)
{
	memcpy((uint8_t *)b->map + (va - b->va), w, n * 4);
}

/* The transfer context and its static programs, as powervr_sgx_blit. */
static void transfer_setup(void)
{
	struct drm_powervr_sgx_ioctl_create_context_args cc = {
		.type = DRM_POWERVR_SGX_CTX_TYPE_TRANSFER, .vm_context_handle = dev.vm,
	};
	struct drm_syncobj_create sc = {};
	uint32_t sec[SGXTEX_SECONDARY_WORDS], nop;
	int e;

	if ((e = xioctl(DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &cc)) || (e = xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc)))
		die("transfer context", e);
	xctx = cc.handle;
	xsync = sc.handle;
	sgx_bo_alloc(&dev, &usse, HEAP_USSE_PIXEL, 0, 0x20000, 0x1000);	/* a code object */
	sgx_bo_alloc(&dev, &bgb, HEAP_GENERAL, 0, 0x4800, 0x1000);
	sgx_bo_alloc(&dev, &pdsc, HEAP_PDS_PIXEL, 0, 0x1800, 0x1000);
	sgx_bo_alloc(&dev, &pdsp, HEAP_PDS_PIXEL, 0, 0x400, 0x1000);
	sgx_bo_alloc(&dev, &copy, HEAP_GENERAL, 0, W * H * 4, 0x1000);
	X.bg = bgb.va;
	X.pds_secondary = pdsc.va;
	X.pds_event = pdsc.va + 0x20;
	X.pds_primary = pdsp.va;
	emit = usse.va + 0x80;
	nop = usse.va + 0xc0;
	X.copy_use = usse.va + 0x100;
	put(&usse, emit, sgxtex_emit_shader, 6);
	put(&usse, nop, sgxtex_nop_shader, 4);
	put(&usse, X.copy_use, sgxtex_copy_shader, 4);
	if ((e = sgx_bo_upload(&dev, &usse, 0, 0x200)))
		die("UPLOAD_CODE", e);
	eot_next = 0x200;
	sgxtex_xctx_secondary(sec, nop);
	put(&pdsc, X.pds_secondary, sec, SGXTEX_SECONDARY_WORDS);
}

/* Each blit's end-of-tile program at the next 128 bytes: a mapped code object only grows. */
static void eot_place(void)
{
	uint32_t ev[SGXTEX_EVENT_WORDS];

	if (eot_next + 0x80 > usse.size)
		die("out of code space", ENOSPC);
	X.eot_use = usse.va + eot_next;
	eot_next += 0x80;
	sgxtex_xctx_pixel_event(ev, X.eot_use, emit);
	put(&pdsc, X.pds_event, ev, SGXTEX_EVENT_WORDS);
}

/* A transfer job copying surface s to `copy`, waiting on `wait` (0: nothing). */
struct xjob {
	uint32_t s[12];
	struct drm_powervr_sgx_sync_op op[2];
	struct drm_powervr_sgx_job job;
};

static void transfer_job(struct xjob *x, const struct sgx_surface *s, uint32_t wait)
{
	struct sgxtex_surface src = { s->color.va, W, H, s->stride, SGXTEX_LAYOUT_STRIDED, SGXTEX_FMT_BGRA8888,
				      SGXPBE_FMT_BGRA8888 };
	struct sgxtex_surface dst = { copy.va, W, H, W, SGXTEX_LAYOUT_STRIDED, SGXTEX_FMT_BGRA8888,
				      SGXPBE_FMT_BGRA8888 };
	struct sgxtex_blit b;
	uint32_t n = 0;
	int e;

	eot_place();
	sgxtex_blit_build(&b, &X, &src, &dst, SGXTEX_FILTER_NEAREST);
	put(&bgb, X.bg, b.bg, SGXTEX_BLIT_BG_WORDS);
	put(&pdsp, X.pds_primary, b.pds, SGXTEX_BLIT_PDS_WORDS);
	put(&usse, X.eot_use, b.eot, SGXPBE_EOT_WORDS);
	if ((e = sgx_bo_upload(&dev, &usse, X.eot_use - usse.va, SGXPBE_EOT_WORDS * 4)))
		die("UPLOAD_CODE", e);
	__sync_synchronize();

	memset(x, 0, sizeof(*x));
	x->s[0] = 48;			/* CMD_TRANSFER (UAPI.md 3.5) */
	x->s[2] = X.bg;
	x->s[4] = b.cmd[9];
	x->s[6] = b.cmd[10];
	x->s[7] = b.cmd[12] >> 16;
	x->s[8] = b.cmd[12] & 0xffff;
	x->s[9] = b.cmd[13] >> 16;
	x->s[10] = b.cmd[13] & 0xffff;
	x->s[11] = b.cmd[2];
	if (wait)
		x->op[n++] = (struct drm_powervr_sgx_sync_op){ .handle = wait,
							       .flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_WAIT };
	x->op[n++] = (struct drm_powervr_sgx_sync_op){ .handle = xsync, .flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL };
	x->job = (struct drm_powervr_sgx_job){
		.type = DRM_POWERVR_SGX_JOB_TYPE_TRANSFER, .context_handle = xctx,
		.cmd_stream = (uintptr_t)x->s, .cmd_stream_len = 48,
		.sync_ops = OBJ_ARRAY(n, x->op),
	};
}

static int submit(struct drm_powervr_sgx_job *j, uint32_t n)
{
	struct drm_powervr_sgx_job a[2];
	struct drm_powervr_sgx_ioctl_submit_jobs_args sj = { .jobs = OBJ_ARRAY(n, a) };

	memcpy(a, j, n * sizeof(*j));
	return xioctl(DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &sj);
}

/* One render context with its target and shaders. */
struct rc {
	struct sgx_ctx *c;
	struct sgx_surface *s;
	struct sgx_shader *vs, *fs;
};

static void rc_create(struct rc *r)
{
	r->c = sgx_ctx_create(&dev);
	r->s = sgx_surface_create(r->c, W, H, 0);
	r->vs = sgx_shader_builtin(r->c, SGX_VS_XY);
	r->fs = sgx_shader_builtin(r->c, SGX_FS_UNIFORM);
}

static const uint32_t want_bg = 0xff000080;

static uint32_t tri_colour(unsigned f)
{
	return f & 1 ? 0xffffff00 : 0xff00ff00;
}

/* Record a clear and a triangle (colour by frame number) into r's frame. */
static void record(struct rc *r, unsigned f)
{
	static const struct sgx_state st = { .front = SGX_CCW };
	static const float clear[4] = { 0, 0, 0.5f, 1 };
	static const float tri[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f };
	struct sgx_draw d = {
		.prim = SGX_TRIANGLES, .count = 3, .verts = tri, .state = &st, .vs = r->vs, .fs = r->fs,
		.color = { f & 1 ? 1.0f : 0, 1, 0, 1 },
	};
	int e;

	memset(r->s->color.map, 0, r->s->stride * H * 4);
	sgx_set_framebuffer(r->c, r->s);
	sgx_clear(r->c, clear, 1.0f);
	if ((e = sgx_draw(r->c, &d)))
		die("draw", e);
}

static int frame_ok(const char *name, const uint32_t *p, uint32_t pitch, unsigned f)
{
	uint32_t centre = p[H / 2 * pitch + W / 2], corner = p[2 * pitch + 2];
	int ok = centre == tri_colour(f) && corner == want_bg;

	if (verbose || !ok)
		printf("  %s: centre %08x (want %08x), corner %08x (want %08x)\n", name, centre, tri_colour(f),
		       corner, want_bg);
	return ok;
}

static int render(struct rc *r, unsigned f)
{
	record(r, f);
	return !sgx_flush(r->c, 4000) && frame_ok("frame", r->s->color.map, r->s->stride, f);
}

static int blit_ok(const struct sgx_surface *s)
{
	const uint32_t *a = s->color.map, *b = copy.map;
	int bad = 0;

	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++)
			bad += a[y * s->stride + x] != b[y * W + x];
	if (verbose || bad)
		printf("  copy: %d/%d pixels differ from the frame\n", bad, W * H);
	return !bad;
}

/* The render and the transfer that copies it, in one SUBMIT_JOBS. */
static int mixed(struct rc *r, unsigned f)
{
	struct sgx_job rj;
	struct xjob x;
	struct drm_powervr_sgx_job j[2];
	struct drm_syncobj_array rs = { .handles = (uintptr_t)&xsync, .count_handles = 1 };
	int e;

	record(r, f);
	memset(copy.map, 0, W * H * 4);
	xioctl(DRM_IOCTL_SYNCOBJ_RESET, &rs);
	sgx_render_job(r->c, &rj);
	transfer_job(&x, r->s, r->c->syncobj);
	j[0] = rj.job;
	j[1] = x.job;
	if ((e = submit(j, 2)))
		die("mixed SUBMIT_JOBS", e);
	if ((e = wait_syncobj(xsync, 4000))) {
		printf("  mixed: transfer not done: %d\n", e);
		return 0;
	}
	return frame_ok("mixed frame", r->s->color.map, r->s->stride, f) &&
	       frame_ok("mixed copy", copy.map, W, f) && blit_ok(r->s);
}

static int blit_alone(struct rc *r)
{
	struct xjob x;
	struct drm_syncobj_array rs = { .handles = (uintptr_t)&xsync, .count_handles = 1 };
	int e;

	memset(copy.map, 0, W * H * 4);
	xioctl(DRM_IOCTL_SYNCOBJ_RESET, &rs);
	transfer_job(&x, r->s, 0);
	if ((e = submit(&x.job, 1)))
		die("transfer SUBMIT_JOBS", e);
	return !wait_syncobj(xsync, 4000) && blit_ok(r->s);
}

static struct drm_powervr_sgx_ioctl_get_context_state_args state(uint32_t ctx)
{
	struct drm_powervr_sgx_ioctl_get_context_state_args a = { .handle = ctx };
	int e;

	if ((e = xioctl(DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE, &a)))
		die("GET_CONTEXT_STATE", e);
	return a;
}

/*
 * The hang's devcoredump: a devcd<N>/data that starts with "powervr_sgx hang:".
 * Freed (written to) once read, so the next hang can make one. Returns its size.
 */
static long coredump_take(void)
{
	DIR *d = opendir("/sys/class/devcoredump");
	struct dirent *e;
	long size = -1;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char path[300], buf[4096];
		long n, total = 0;
		int f;

		if (strncmp(e->d_name, "devcd", 5))
			continue;
		snprintf(path, sizeof(path), "/sys/class/devcoredump/%s/data", e->d_name);
		f = open(path, O_RDONLY);
		if (f < 0)
			continue;
		n = read(f, buf, sizeof(buf));
		if (n > 18 && !strncmp(buf, "powervr_sgx hang:", 17)) {
			total = n;
			if (verbose)
				printf("  %.*s", (int)(strchr(buf, '\n') ? strchr(buf, '\n') - buf + 1 : 0), buf);
			while ((n = read(f, buf, sizeof(buf))) > 0)
				total += n;
			size = total;
		}
		close(f);
		if (size >= 0) {
			f = open(path, O_WRONLY);
			if (f >= 0) {
				if (write(f, "1", 1) != 1)
					size = -2;
				close(f);
			}
			break;
		}
	}
	closedir(d);
	return size;
}

/* power/runtime_status of the GPU's PCI function. */
static int rpm_status(char *buf, size_t n)
{
	FILE *fp = fopen("/sys/bus/pci/devices/0000:01:02.0/power/runtime_status", "r");
	int ok;

	if (!fp)
		return -1;
	ok = fgets(buf, (int)n, fp) != NULL;
	fclose(fp);
	if (ok)
		buf[strcspn(buf, "\n")] = 0;
	return ok ? 0 : -1;
}

/* 1 once suspended, 0 if not within ms, -1 without runtime PM. */
static int rpm_wait_suspended(unsigned ms)
{
	FILE *fp = fopen("/sys/bus/pci/devices/0000:01:02.0/power/control", "r");
	char st[32] = "";

	/* "on": runtime PM forbidden (powervr_sgx.runtime_pm=0) */
	if (fp) {
		if (!fgets(st, sizeof(st), fp))
			st[0] = 0;
		fclose(fp);
		if (!strncmp(st, "on", 2))
			return -1;
	}
	for (unsigned t = 0; t <= ms; t += 20) {
		if (rpm_status(st, sizeof(st)) || !strcmp(st, "unsupported"))
			return -1;
		if (!strcmp(st, "suspended"))
			return 1;
		usleep(20000);
	}
	return 0;
}

#define CHECK(cond) do { if (!(cond)) { printf("  FAILED: %s\n", #cond); bad++; } } while (0)

static int hang(struct rc *a, unsigned f)
{
	struct drm_powervr_sgx_ioctl_get_context_state_args s0, sb;
	struct rc b, c;
	struct sgx_job j;
	int bad = 0, e;
	long dump;

	coredump_take();	/* none left from before */
	rc_create(&b);
	s0 = state(b.c->ctx);
	setenv("SGXGEN_FAULT", "1", 1);
	record(&b, f);
	unsetenv("SGXGEN_FAULT");
	e = sgx_flush(b.c, 10000);
	sb = state(b.c->ctx);
	printf("  faulting render: wait %d, B state %u, resets %u -> %u\n", e, sb.state, s0.reset_count,
	       sb.reset_count);
	CHECK(sb.state == DRM_POWERVR_SGX_CONTEXT_STATE_GUILTY);
	CHECK(sb.reset_count == s0.reset_count + 1);
	CHECK(state(a->c->ctx).state == DRM_POWERVR_SGX_CONTEXT_STATE_OK);
	CHECK(state(xctx).state == DRM_POWERVR_SGX_CONTEXT_STATE_OK);
	dump = coredump_take();
	printf("  devcoredump: %ld bytes\n", dump);
	CHECK(dump > 1000);

	/* B is refused from now on. */
	record(&b, f);
	sgx_render_job(b.c, &j);
	CHECK(submit(&j.job, 1) == -ECANCELED);

	CHECK(render(a, f));
	rc_create(&c);
	CHECK(render(&c, f + 1));
	CHECK(blit_alone(&c));
	CHECK(mixed(a, f + 1));
	sgx_surface_destroy(c.c, c.s);
	sgx_ctx_destroy(c.c);
	sgx_surface_destroy(b.c, b.s);
	sgx_ctx_destroy(b.c);
	return !bad;
}

static int idle(struct rc *a, unsigned f)
{
	char st[32] = "";
	int64_t t0;
	int bad = 0, s, ok;

	s = rpm_wait_suspended(3000);
	if (s < 0) {
		printf("  no runtime PM\n");
		return 1;
	}
	CHECK(s == 1);
	t0 = deadline(0);
	ok = render(a, f);
	printf("  render from suspended: %lld us\n", (long long)(deadline(0) - t0) / 1000);
	CHECK(ok);
	rpm_status(st, sizeof(st));
	CHECK(!strcmp(st, "active"));
	CHECK(rpm_wait_suspended(3000) == 1);
	CHECK(mixed(a, f + 1));
	CHECK(rpm_wait_suspended(3000) == 1);
	CHECK(blit_alone(a));
	return !bad;
}

int main(int argc, char **argv)
{
	unsigned rounds = 1, bad = 0, f = 0;
	int opt, e, do_hang = 1;
	struct rc a;

	while ((opt = getopt(argc, argv, "vn:H")) != -1) {
		switch (opt) {
		case 'v': verbose = 1; break;
		case 'n': rounds = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'H': do_hang = 0; break;
		default:
			fprintf(stderr, "usage: powervr_sgx_hang [-v] [-n rounds] [-H]\n");
			return 2;
		}
	}
	if ((e = sgx_dev_open(&dev, NULL, NULL))) {
		fprintf(stderr, "powervr_sgx_hang: open: %d\n", e);
		return 77;
	}
	dev.verbose = verbose;
	transfer_setup();
	rc_create(&a);

	for (unsigned r = 0; r < rounds; r++, f += 2) {
		int ok = render(&a, f);

		printf("round %u: render %s\n", r, ok ? "right" : "WRONG");
		bad += !ok;
		ok = mixed(&a, f + 1);
		printf("round %u: mixed render + transfer %s\n", r, ok ? "right" : "WRONG");
		bad += !ok;
		ok = idle(&a, f);
		printf("round %u: idle (runtime PM) %s\n", r, ok ? "right" : "WRONG");
		bad += !ok;
		if (do_hang) {
			ok = hang(&a, f);
			printf("round %u: hang recovery %s\n", r, ok ? "right" : "WRONG");
			bad += !ok;
		}
	}
	sgx_surface_destroy(a.c, a.s);
	sgx_ctx_destroy(a.c);
	sgx_dev_close(&dev);
	printf("%s\n", bad ? "FAILED" : "all right");
	return bad ? 1 : 0;
}
