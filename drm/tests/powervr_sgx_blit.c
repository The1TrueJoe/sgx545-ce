// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * Transfer jobs through powervr_sgx: tools/tex's blit builder (sgxtex) writes the
 * background block, the pixel program and the end-of-tile program into client
 * memory, and one SUBMIT_JOBS transfer job with a CMD_TRANSFER stream runs each
 * blit (docs/TEX.md 3 and 10). Every destination byte is checked.
 *
 *   powervr_sgx_blit [-v] [-n rounds] [-T] [-2]
 *
 * -T puts the sampled source in the texture heap instead of the general heap.
 * -2 first runs one round in a process of its own, to its exit, then the rounds
 * in a new one: the second client's programs sit at the first's addresses, so
 * the GPU's caches must not keep the first's (released-context invalidate).
 *
 * Cases, 64x64 BGRA8888 from a CPU-written pattern:
 *   copy   strided  -> strided            exact copy
 *   up     strided  -> twiddled           (texture upload)
 *   down   twiddled -> linear             the upload read back: equals the pattern
 *   mipl   twiddled -> 32x32 strided, LINEAR  box filter
 *
 * Exit 0 when every byte is right; 77 when there is no powervr_sgx device.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>

#include "powervr_sgx_drm.h"
#include "sgxtex.h"

int ioctl(int fd, unsigned long req, ...);

#define DRM_POWERVR_SGX_OBJ_ARRAY(cnt, ptr) \
	{ .stride = sizeof((ptr)[0]), .count = (cnt), .array = (__u64)(uintptr_t)(ptr) }

static int fd, verbose;
static uint32_t vm, ctx, syncobj;
static uint64_t heap_base[DRM_POWERVR_SGX_HEAP_COUNT], heap_next[DRM_POWERVR_SGX_HEAP_COUNT];
/* -T: the sampled source in the texture heap, as the sample-cache erratum wants. */
static int src_heap = DRM_POWERVR_SGX_HEAP_GENERAL;

static int xioctl(unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r == -1 ? -errno : 0;
}

static void die(const char *what, int e)
{
	fprintf(stderr, "powervr_sgx_blit: %s: %d\n", what, e);
	exit(1);
}

struct buf {
	uint32_t va, handle, size;
	uint8_t *cpu;	/* a code object's: a CPU copy, uploaded with upload() */
};

/*
 * A buffer object at the next free address of a heap: CPU-mapped, or in a
 * USSE heap a code object with a CPU copy.
 */
static struct buf bo(int heap, uint32_t size)
{
	int code = heap == DRM_POWERVR_SGX_HEAP_USSE_PIXEL || heap == DRM_POWERVR_SGX_HEAP_USSE_VERTEX;
	struct drm_powervr_sgx_ioctl_create_bo_args cb = {
		.size = (size + 0xfff) & ~0xfffu,
		.flags = code ? DRM_POWERVR_SGX_BO_CODE : DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS,
	};
	struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args mo = {};
	struct drm_powervr_sgx_vm_bind_op op = { .op = DRM_POWERVR_SGX_VM_BIND_OP_MAP };
	struct drm_powervr_sgx_ioctl_vm_bind_args m = {
		.flags = DRM_POWERVR_SGX_VM_BIND_FLAG_SYNC,
		.ops = { .stride = sizeof(op), .count = 1, .array = (uintptr_t)&op },
	};
	struct buf b;
	int e;

	if (!heap_next[heap])
		heap_next[heap] = heap_base[heap] + 0x1000;
	if ((e = xioctl(DRM_IOCTL_POWERVR_SGX_CREATE_BO, &cb)))
		die("CREATE_BO", e);
	if (code) {
		b.cpu = calloc(1, cb.size);
		if (!b.cpu)
			die("calloc", ENOMEM);
	} else {
		mo.handle = cb.handle;
		if ((e = xioctl(DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &mo)))
			die("GET_BO_MMAP_OFFSET", e);
		b.cpu = mmap(NULL, cb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mo.offset);
		if (b.cpu == MAP_FAILED)
			die("mmap", errno);
		memset(b.cpu, 0, cb.size);
	}
	b.handle = cb.handle;
	b.size = cb.size;
	m.vm_context_handle = vm;
	op.handle = cb.handle;
	op.device_addr = heap_next[heap];
	op.size = cb.size;
	if ((e = xioctl(DRM_IOCTL_POWERVR_SGX_VM_BIND, &m)))
		die("VM_BIND", e);
	b.va = (uint32_t)heap_next[heap];
	heap_next[heap] += cb.size;
	return b;
}

static void put(struct buf *b, uint32_t va, const uint32_t *w, uint32_t n)
{
	memcpy(b->cpu + (va - b->va), w, n * 4);
}

/* A code object's bytes [va, va + bytes) to the GPU. */
static void upload(struct buf *b, uint32_t va, uint32_t bytes)
{
	struct drm_powervr_sgx_ioctl_upload_code_args a = {
		.handle = b->handle, .offset = va - b->va, .size = (bytes + 7) & ~7u,
		.pointer = (uintptr_t)(b->cpu + (va - b->va)),
	};
	int e = xioctl(DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE, &a);

	if (e) {
		fprintf(stderr, "powervr_sgx_blit: UPLOAD_CODE rejected offset %#llx\n",
			(unsigned long long)a.bad_offset);
		die("UPLOAD_CODE", e);
	}
}

/* The context's static programs, as blitest's context() (TEX.md 10). */
static struct sgxtex_blit_ctx X;
static struct buf usse, bgb, pdsc, pdsp;
static uint32_t emit, eot_next;

/*
 * Each blit's end-of-tile program goes to the next 128 bytes of the code
 * object: once mapped, a code object only takes uploads past its end.
 */
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

static void programs(void)
{
	uint32_t sec[SGXTEX_SECONDARY_WORDS], nop;

	usse = bo(DRM_POWERVR_SGX_HEAP_USSE_PIXEL, 0x20000);
	bgb = bo(DRM_POWERVR_SGX_HEAP_GENERAL, 0x4800);
	pdsc = bo(DRM_POWERVR_SGX_HEAP_PDS_PIXEL, 0x1800);
	pdsp = bo(DRM_POWERVR_SGX_HEAP_PDS_PIXEL, 0x400);
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
	upload(&usse, usse.va, 0x200);
	eot_next = 0x200;
	sgxtex_xctx_secondary(sec, nop);
	put(&pdsc, X.pds_secondary, sec, SGXTEX_SECONDARY_WORDS);
}

/* One blit: sgxtex's words into client memory, the rest as a CMD_TRANSFER stream. */
static int blit(const struct sgxtex_surface *src, const struct sgxtex_surface *dst, int filter)
{
	struct sgxtex_blit b;
	uint32_t s[12] = { 48 };
	struct drm_powervr_sgx_sync_op op = {
		.handle = syncobj,
		.flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL |
			 DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
	};
	struct drm_powervr_sgx_job j = {
		.type = DRM_POWERVR_SGX_JOB_TYPE_TRANSFER, .context_handle = ctx,
		.flags = dst->layout == SGXTEX_LAYOUT_TWIDDLED ? DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_DST_TWIDDLED : 0,
		.cmd_stream = (uintptr_t)s, .cmd_stream_len = sizeof(s),
		.sync_ops = DRM_POWERVR_SGX_OBJ_ARRAY(1, &op),
	};
	struct drm_powervr_sgx_ioctl_submit_jobs_args a = { .jobs = DRM_POWERVR_SGX_OBJ_ARRAY(1, &j) };
	struct drm_syncobj_wait w = {
		.handles = (uintptr_t)&syncobj, .count_handles = 1, .timeout_nsec = INT64_MAX,
	};
	struct drm_syncobj_array r = { .handles = (uintptr_t)&syncobj, .count_handles = 1 };
	struct timespec now;
	int e;

	eot_place();
	sgxtex_blit_build(&b, &X, src, dst, filter);
	put(&bgb, X.bg, b.bg, SGXTEX_BLIT_BG_WORDS);
	put(&pdsp, X.pds_primary, b.pds, SGXTEX_BLIT_PDS_WORDS);
	put(&usse, X.eot_use, b.eot, SGXPBE_EOT_WORDS);
	upload(&usse, X.eot_use, SGXPBE_EOT_WORDS * 4);
	__sync_synchronize();

	/* CMD_TRANSFER (docs/UAPI.md 3.5): header, then the words the kernel turns into the command */
	s[2] = X.bg;				/* bg_block_addr */
	s[4] = b.cmd[9];			/* pixel_event_pds_addr */
	s[6] = b.cmd[10];			/* pixel_event_pds_rows */
	s[7] = b.cmd[12] >> 16;			/* first tile */
	s[8] = b.cmd[12] & 0xffff;
	s[9] = b.cmd[13] >> 16;			/* last tile */
	s[10] = b.cmd[13] & 0xffff;
	s[11] = b.cmd[2];			/* bg_depth */

	xioctl(DRM_IOCTL_SYNCOBJ_RESET, &r);
	if ((e = xioctl(DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &a)))
		die("SUBMIT_JOBS", e);
	clock_gettime(CLOCK_MONOTONIC, &now);
	w.timeout_nsec = now.tv_sec * 1000000000ll + now.tv_nsec + 2000000000ll;
	return xioctl(DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

static uint32_t pattern(uint32_t x, uint32_t y)
{
	return 0xff000000 | (x * 4) << 16 | (y * 4) << 8 | ((x ^ y) * 4 & 0xff);
}

/* Bytes of a strided/linear BGRA8888 surface that differ from want(). */
static int check(const char *name, int e, const struct buf *b, uint32_t w, uint32_t h, uint32_t pitch,
		 uint32_t (*want)(uint32_t, uint32_t))
{
	const uint32_t *p = (const uint32_t *)b->cpu;
	int bad = 0;

	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			if (p[y * pitch + x] != want(x, y)) {
				if (verbose && bad < 4)
					printf("  %s (%u,%u) %08x want %08x\n", name, x, y, p[y * pitch + x],
					       want(x, y));
				bad++;
			}
	printf("%-5s %s, %u/%u pixels right\n", name, e ? "TIMED OUT" : "done", w * h - bad, w * h);
	return e || bad;
}

/* LINEAR at half size: (((a+b+1)>>1) + ((c+d+1)>>1) + 1) >> 1 per channel (TEX.md 10). */
static uint32_t box(uint32_t x, uint32_t y)
{
	uint32_t a = pattern(2 * x, 2 * y), b = pattern(2 * x + 1, 2 * y);
	uint32_t c = pattern(2 * x, 2 * y + 1), d = pattern(2 * x + 1, 2 * y + 1), v = 0;

	for (int s = 0; s < 32; s += 8) {
		uint32_t ab = (((a >> s) & 0xff) + ((b >> s) & 0xff) + 1) >> 1;
		uint32_t cd = (((c >> s) & 0xff) + ((d >> s) & 0xff) + 1) >> 1;

		v |= ((ab + cd + 1) >> 1) << s;
	}
	return v;
}

int main(int argc, char **argv)
{
	struct drm_powervr_sgx_heap h[DRM_POWERVR_SGX_HEAP_COUNT];
	struct drm_powervr_sgx_dev_query_heap_info info = {
		.heaps = { .stride = sizeof(h[0]), .count = DRM_POWERVR_SGX_HEAP_COUNT, .array = (uintptr_t)h },
	};
	struct drm_powervr_sgx_ioctl_dev_query_args q = {
		.type = DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET, .size = sizeof(info), .pointer = (uintptr_t)&info,
	};
	struct drm_powervr_sgx_ioctl_create_vm_context_args cv = {};
	struct drm_powervr_sgx_ioctl_create_context_args cc = { .type = DRM_POWERVR_SGX_CTX_TYPE_TRANSFER };
	struct drm_syncobj_create sc = {};
	struct buf src, sb, tw, lin, mip;
	struct sgxtex_surface s_src = { 0, 64, 64, 64, SGXTEX_LAYOUT_STRIDED, SGXTEX_FMT_BGRA8888,
					SGXPBE_FMT_BGRA8888 };
	struct sgxtex_surface s_copy = s_src, s_tw = s_src, s_lin = s_src, s_mip = s_src;
	unsigned rounds = 1;
	int opt, e, bad = 0, two = 0;

	while ((opt = getopt(argc, argv, "vn:T2")) != -1) {
		switch (opt) {
		case 'v': verbose = 1; break;
		case 'T': src_heap = DRM_POWERVR_SGX_HEAP_TEXTURE; break;
		case 'n': rounds = (unsigned)strtoul(optarg, NULL, 0); break;
		case '2': two = 1; break;
		default:
			fprintf(stderr, "usage: powervr_sgx_blit [-v] [-n rounds] [-T] [-2]\n");
			return 2;
		}
	}
	if (two) {
		char *args[] = { argv[0], "-n", "1",
				 src_heap == DRM_POWERVR_SGX_HEAP_TEXTURE ? "-T" : NULL, NULL };
		pid_t pid = fork();
		int status;

		if (pid < 0)
			die("fork", -errno);
		if (!pid) {
			execv("/proc/self/exe", args);
			_exit(127);
		}
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
			die("first client", -1);
		if (WEXITSTATUS(status) == 77)
			return 77;
		printf("first client: %s\n", WEXITSTATUS(status) ? "FAILED" : "all blits right");
		bad |= WEXITSTATUS(status) != 0;
	}
	fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd < 0 || xioctl(DRM_IOCTL_POWERVR_SGX_DEV_QUERY, &q))
		return 77;
	for (int i = 0; i < DRM_POWERVR_SGX_HEAP_COUNT; i++)
		heap_base[i] = h[i].base;
	if ((e = xioctl(DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT, &cv)))
		die("CREATE_VM_CONTEXT", e);
	vm = cc.vm_context_handle = cv.handle;
	if ((e = xioctl(DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &cc)) ||
	    (e = xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc)))
		die("CREATE_CONTEXT", e);
	ctx = cc.handle;
	syncobj = sc.handle;
	programs();

	src = bo(src_heap, 64 * 64 * 4);
	sb = bo(DRM_POWERVR_SGX_HEAP_GENERAL, 64 * 64 * 4);
	tw = bo(DRM_POWERVR_SGX_HEAP_TEXTURE, 64 * 64 * 4);
	lin = bo(DRM_POWERVR_SGX_HEAP_GENERAL, 64 * 64 * 4);
	mip = bo(DRM_POWERVR_SGX_HEAP_GENERAL, 32 * 32 * 4);
	for (uint32_t y = 0; y < 64; y++)
		for (uint32_t x = 0; x < 64; x++)
			((uint32_t *)src.cpu)[y * 64 + x] = pattern(x, y);
	s_src.addr = src.va;
	s_copy.addr = sb.va;
	s_tw.addr = tw.va;
	s_tw.layout = SGXTEX_LAYOUT_TWIDDLED;
	s_lin.addr = lin.va;
	s_lin.layout = SGXTEX_LAYOUT_LINEAR;
	s_mip.addr = mip.va;
	s_mip.w = s_mip.h = s_mip.pitch = 32;

	for (unsigned r = 0; r < rounds; r++) {
		memset(sb.cpu, 0, 64 * 64 * 4);
		memset(tw.cpu, 0, 64 * 64 * 4);
		memset(lin.cpu, 0, 64 * 64 * 4);
		memset(mip.cpu, 0, 32 * 32 * 4);
		e = blit(&s_src, &s_copy, SGXTEX_FILTER_NEAREST);
		bad |= check("copy", e, &sb, 64, 64, 64, pattern);
		e = blit(&s_src, &s_tw, SGXTEX_FILTER_NEAREST);
		e |= blit(&s_tw, &s_lin, SGXTEX_FILTER_NEAREST);
		bad |= check("down", e, &lin, 64, 64, sgxtex_pitch(64), pattern);
		e = blit(&s_tw, &s_mip, SGXTEX_FILTER_LINEAR);
		bad |= check("mipl", e, &mip, 32, 32, 32, box);
	}
	printf("%s\n", bad ? "FAILED" : "all blits right");
	return bad ? 1 : 0;
}
