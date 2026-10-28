/*
 * blitest: GPU blits on a transfer context of our own, every word from sgxtex.
 *
 * No capture and no DDK: it allocates the context's buffers the way libsrv_um
 * does, registers the context, writes the static programs and then runs one
 * SGX_SUBMITTRANSFER per case, waiting on the destination's sync object.
 * Each destination is dumped to <outdir>/<case>.bin for check_blit.py, which
 * predicts every byte.
 *
 *   blitest <outdir>          (tools/tex/build.sh builds it static for the EA3)
 *
 * Cases (the source is a fixed pattern, see pattern()):
 *   up      64x64 strided  -> 64x64 twiddled      (texture upload)
 *   down    64x64 twiddled -> 64x64 linear        (readback / glCopyTexImage from a texture)
 *   mipl    64x64 twiddled -> 32x32 twiddled, LINEAR   (mip level, box filter)
 *   mipn    64x64 twiddled -> 32x32 twiddled, NEAREST  (mip level as the DDK makes it)
 *   up2x    64x64 strided  -> 128x128 strided, NEAREST (scaled copy)
 *   npot    100x30 linear  -> 100x30 strided           (NPOT texture -> surface)
 *   to565   64x64 strided RGBA8888 -> 64x64 strided RGB565  (format conversion)
 *
 *   blitest <outdir> rect     sub-rectangles (glCopyTexSubImage, glBlitFramebuffer):
 *     rsub    (8,4) 24x20 of the strided source -> (30,40) of a strided 64x64 filled with 0x11
 *     rsubtw  (40,10) 16x16 -> (8,24) of a twiddled 64x64 filled with 0x22: sgxtex refuses it
 *             now (see rect2); before the check the PBE wrote it to the wrong places
 *     rscale  (0,0) 16x8 -> (10,10) 32x16 of a strided 64x64 filled with 0x11, NEAREST (2x)
 *   blitest <outdir> rect2    twiddled destinations: the PBE takes a twiddled surface's tile
 *                             row length from the clip width, so only full-width bands clip:
 *     rband   rows 10..41 of the source -> rows 16..47 of a twiddled 64x64 (tile-aligned band)
 *     rband2  rows 10..25 -> rows 20..35 (unaligned band)
 *     rcomp   (40,10) 16x16 -> (8,24) of a twiddled 64x64 in three blits: readback to linear,
 *             sub-rectangle into it, upload
 *   blitest <outdir> present  the swap: a 720x480 BGRA8888 back buffer (pitch 736) filled
 *                             with a pattern, blitted into the real /dev/fb0 (wrapped as the
 *                             DDK's EGL does); prints the blit's time. Read fb0 back after.
 *   blitest <outdir> sweep    one 64x64 strided copy per variant of the background
 *                             object's vertex words (bg[16..22]), into a cleared
 *                             destination: sweep<n>.bin, for working out sub-rectangles
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pvr.h"
#include "sgxtex.h"

#define H_GENERAL 0
#define H_KDATA 3
#define H_PIXSHADER 4
#define H_PDSPIXEL 6
#define H_TEXTURE 14

static struct pvr P;

#define die(...) do { fprintf(stderr, "blitest: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static void bo(struct pvr_buf *b, uint32_t heap, uint32_t attribs, uint32_t size, uint32_t align)
{
	int e = pvr_alloc(&P, b, heap, attribs, size, align);

	if (e)
		die("ALLOC_DEVICEMEM heap %u size %#x: %d", heap, size, e);
	if ((e = pvr_map(&P, b)))
		die("map %#x: %d", b->devva, e);
	memset(b->cpu, 0, size);
}

static void put(struct pvr_buf *b, uint32_t va, const uint32_t *w, size_t n)
{
	if (va < b->devva || va + 4 * n > b->devva + b->size)
		die("%#x+%zu words outside buffer %#x", va, n, b->devva);
	memcpy((char *)b->cpu + (va - b->devva), w, 4 * n);
}

static void msleep(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

	nanosleep(&ts, NULL);
}

/* the context: its buffers and where each program lives */
static struct pvr_buf ccb, ctl, usse, bgb, pdsc, pdsp;
static struct sgxtex_blit_ctx X;
static uint32_t xctx_va, woff;

static void context(void)
{
	uint32_t words[SGXTEX_XCTX_BYTES / 4], sec[SGXTEX_SECONDARY_WORDS], ev[SGXTEX_EVENT_WORDS];
	uint32_t emit, nop;
	IMG_HANDLE h;
	int e;

	bo(&ccb, H_KDATA, 0x4f, 0x10000 + 0x4c8, 0x1000);
	bo(&ctl, H_KDATA, 0x4f, 8, 0x10);
	bo(&usse, H_PIXSHADER, 0xd, 0x800, 0x80);
	bo(&bgb, H_GENERAL, 0xd, 0x4800, 0x40);
	bo(&pdsc, H_PDSPIXEL, 0xd, 0x1800, 0x10);
	bo(&pdsp, H_PDSPIXEL, 0xd, 0x400, 0x10);

	X.bg = bgb.devva;
	X.pds_secondary = pdsc.devva;
	X.pds_event = pdsc.devva + 0x20;
	X.pds_primary = pdsp.devva;
	X.eot_use = usse.devva;
	emit = usse.devva + 0x80;
	nop = usse.devva + 0xc0;
	X.copy_use = usse.devva + 0x100;

	put(&usse, emit, sgxtex_emit_shader, 6);
	put(&usse, nop, sgxtex_nop_shader, 4);
	put(&usse, X.copy_use, sgxtex_copy_shader, 4);
	sgxtex_xctx_secondary(sec, nop);
	put(&pdsc, X.pds_secondary, sec, SGXTEX_SECONDARY_WORDS);
	sgxtex_xctx_pixel_event(ev, X.eot_use, emit);
	put(&pdsc, X.pds_event, ev, SGXTEX_EVENT_WORDS);

	sgxtex_xctx_words(words, ccb.devva, ctl.devva, (uint32_t)getpid());
	if ((e = pvr_register_transfer_ctx(&P, words, SGXTEX_XCTX_BYTES, 8, &h, &xctx_va)))
		die("SGX_REGISTER_HW_TRANSFER_CONTEXT: %d", e);
	printf("blitest: transfer context at %#x (ccb %#x, eot %#x, pds %#x/%#x/%#x, bg %#x)\n", xctx_va,
	       ccb.devva, X.eot_use, X.pds_secondary, X.pds_event, X.pds_primary, X.bg);
}

/* a sync object's data, mapped once (a second mapping of the same handle fails) */
static volatile PVRSRV_SYNC_DATA *sync_data(IMG_HANDLE h)
{
	static struct { IMG_HANDLE h; struct pvr_buf b; volatile PVRSRV_SYNC_DATA *d; } c[16];
	static int n;

	for (int i = 0; i < n; i++)
		if (c[i].h == h)
			return c[i].d;
	if (n == 16 || !(c[n].d = pvr_sync_data(&P, h, &c[n].b)))
		die("cannot map sync object %p", (void *)h);
	c[n].h = h;
	return c[n++].d;
}

/* background-block words to override before a blit: index, value; index -1 ends */
static int bg_override[8][2] = { { -1, 0 } };

static const struct sgxtex_rect *blit_srect, *blit_drect;	/* set: a sub-rectangle blit */

static int blit(const char *name, struct pvr_buf *sb, const struct sgxtex_surface *src, struct pvr_buf *db,
		const struct sgxtex_surface *dst, int filter)
{
	struct sgxtex_blit b;
	uint32_t cmd[SGXTEX_CMD_BYTES / 4];
	PVRSRV_TRANSFER_SGX_KICK k;
	volatile PVRSRV_SYNC_DATA *sd = sync_data(db->sync);
	unsigned t = 0;
	int e;

	if (blit_srect) {
		if (sgxtex_blit_build_rect(&b, &X, src, blit_srect, dst, blit_drect, filter)) {
			printf("blitest: %-6s refused by sgxtex (twiddled, not full width)\n", name);
			return 0;
		}
	} else
		sgxtex_blit_build(&b, &X, src, dst, filter);
	for (int i = 0; i < 8 && bg_override[i][0] >= 0; i++)
		b.bg[bg_override[i][0]] = (uint32_t)bg_override[i][1];
	put(&bgb, X.bg, b.bg, SGXTEX_BLIT_BG_WORDS);
	put(&pdsp, X.pds_primary, b.pds, SGXTEX_BLIT_PDS_WORDS);
	put(&usse, X.eot_use, b.eot, SGXPBE_EOT_WORDS);
	sgxtex_blit_command_full(cmd, &b, dst->layout == SGXTEX_LAYOUT_TWIDDLED);
	if (woff + SGXTEX_CMD_BYTES > 0x10000)
		die("CCB full");
	put(&ccb, ccb.devva + woff, cmd, SGXTEX_CMD_BYTES / 4);

	memset(&k, 0, sizeof k);
	k.hCCBMemInfo = ccb.mem;
	k.ui32SharedCmdCCBOffset = woff + SGXTEX_CMD_SHARED;
	k.sHWTransferContextDevVAddr.uiAddr = xctx_va;
	if (!sb->sync || !db->sync)
		die("%s: a surface has no sync object", name);
	k.ui32NumSrcSync = 1;
	k.ahSrcSyncInfo[0] = sb->sync;
	k.ui32NumDstSync = 1;
	k.ahDstSyncInfo[0] = db->sync;
	woff += SGXTEX_CMD_BYTES;
	((uint32_t *)ctl.cpu)[0] = woff;	/* the client's write offset */
	__sync_synchronize();
	if ((e = pvr_submit_transfer(&P, &k)))
		die("%s: SGX_SUBMITTRANSFER: %d", name, e);

	while (sd->ui32WriteOpsComplete < sd->ui32WriteOpsPending && t < 2000) {
		msleep(1);
		t++;
	}
	printf("blitest: %-6s %ux%u -> %ux%u: dst write ops %u/%u after %u ms%s\n", name, src->w, src->h, dst->w,
	       dst->h, sd->ui32WriteOpsComplete, sd->ui32WriteOpsPending, t, t >= 2000 ? " (TIMED OUT)" : "");
	return t >= 2000;
}

static void dump(const char *dir, const char *name, struct pvr_buf *b, uint32_t len)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof path, "%s/%s.bin", dir, name);
	if (!(f = fopen(path, "wb")) || fwrite(b->cpu, 1, len, f) != len || fclose(f))
		die("write %s", path);
}

/* source texel (x, y), RGBA bytes: fixed and asymmetric, so any misplaced texel shows */
static void pattern(uint8_t *p, uint32_t x, uint32_t y)
{
	p[0] = (uint8_t)(x * 7 + y * 3);
	p[1] = (uint8_t)(x * x + 5 * y);
	p[2] = (uint8_t)(x * y);
	p[3] = (uint8_t)(255 - 2 * x - y);
}

int main(int argc, char **argv)
{
	struct pvr_buf lin, tw, out, mip, big, npot, o565;
	struct sgxtex_surface s_lin = { 0, 64, 64, 64, SGXTEX_LAYOUT_STRIDED, SGXTEX_FMT_RGBA8888, SGXPBE_FMT_RGBA8888 };
	struct sgxtex_surface s_tw = { 0, 64, 64, 64, SGXTEX_LAYOUT_TWIDDLED, SGXTEX_FMT_RGBA8888, SGXPBE_FMT_RGBA8888 };
	struct sgxtex_surface s_out = s_lin, s_mip = { 0, 32, 32, 32, SGXTEX_LAYOUT_TWIDDLED, SGXTEX_FMT_RGBA8888,
							SGXPBE_FMT_RGBA8888 };
	struct sgxtex_surface s_big = { 0, 128, 128, 128, SGXTEX_LAYOUT_STRIDED, SGXTEX_FMT_RGBA8888, SGXPBE_FMT_RGBA8888 };
	struct sgxtex_surface s_npot = { 0, 100, 30, 104, SGXTEX_LAYOUT_LINEAR, SGXTEX_FMT_RGBA8888, SGXPBE_FMT_RGBA8888 };
	struct sgxtex_surface s_nout = { 0, 100, 30, 104, SGXTEX_LAYOUT_STRIDED, SGXTEX_FMT_RGBA8888, SGXPBE_FMT_RGBA8888 };
	struct sgxtex_surface s_565 = { 0, 64, 64, 64, SGXTEX_LAYOUT_STRIDED, SGXTEX_FMT_RGB565, SGXPBE_FMT_RGB565 };
	const char *dir = argc > 1 ? argv[1] : ".";
	int bad = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (pvr_open(&P, NULL) || pvr_init(&P))
		die("open/init: %s", strerror(errno));
	context();

	/* surfaces need sync objects (attribs without b3, NO_SYNCOBJ): the kick names them */
	bo(&lin, H_GENERAL, 0x3, 64 * 64 * 4, 0x1000);
	bo(&tw, H_TEXTURE, 0x3, 64 * 64 * 4, 0x1000);
	bo(&out, H_GENERAL, 0x3, 64 * 64 * 4, 0x1000);
	bo(&mip, H_TEXTURE, 0x3, 32 * 32 * 4, 0x1000);
	bo(&big, H_GENERAL, 0x3, 128 * 128 * 4, 0x1000);
	bo(&npot, H_TEXTURE, 0x3, 104 * 30 * 4, 0x1000);
	bo(&o565, H_GENERAL, 0x3, 64 * 64 * 2, 0x1000);
	s_lin.addr = lin.devva, s_tw.addr = tw.devva, s_out.addr = out.devva, s_mip.addr = mip.devva;
	s_big.addr = big.devva, s_npot.addr = npot.devva, s_565.addr = o565.devva;

	for (uint32_t y = 0; y < 64; y++)
		for (uint32_t x = 0; x < 64; x++)
			pattern((uint8_t *)lin.cpu + 4 * (y * 64 + x), x, y);
	for (uint32_t y = 0; y < 30; y++)
		for (uint32_t x = 0; x < 100; x++)
			pattern((uint8_t *)npot.cpu + 4 * (y * 104 + x), x, y);
	dump(dir, "src", &lin, 64 * 64 * 4);
	__sync_synchronize();

	if (argc > 2 && !strcmp(argv[2], "present")) {
		struct pvr_buf back, fb;
		struct sgxtex_surface s_back = { 0, 720, 480, 736, SGXTEX_LAYOUT_STRIDED, SGXTEX_FMT_BGRA8888,
						 SGXPBE_FMT_BGRA8888 };
		struct sgxtex_surface s_fb = s_back;
		struct timespec t0, t1;
		int fd = open("/dev/fb0", O_RDWR);
		void *m = fd < 0 ? MAP_FAILED : mmap(NULL, 736 * 480 * 4, PROT_READ | PROT_WRITE,
						     MAP_SHARED | MAP_32BIT, fd, 0);
		int e;

		if (m == MAP_FAILED)
			die("map /dev/fb0: %s", strerror(errno));
		if ((e = pvr_wrap(&P, &fb, m, 736 * 480 * 4)))
			die("WRAP_EXT_MEMORY of fb0: %d", e);
		bo(&back, H_GENERAL, 0x3, 736 * 480 * 4, 0x1000);
		/* B, G, R, A: a gradient with a grid, so a misplaced or torn row shows */
		for (uint32_t y = 0; y < 480; y++)
			for (uint32_t x = 0; x < 736; x++) {
				uint8_t *p = (uint8_t *)back.cpu + 4 * (y * 736 + x);

				p[0] = (uint8_t)(y * 255 / 479), p[1] = (uint8_t)(x * 255 / 719);
				p[2] = (x % 40 == 0 || y % 40 == 0) ? 255 : (uint8_t)((x ^ y) & 0x7f), p[3] = 255;
			}
		dump(dir, "back", &back, 736 * 480 * 4);
		__sync_synchronize();
		s_back.addr = back.devva, s_fb.addr = fb.devva;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		bad |= blit("swap", &back, &s_back, &fb, &s_fb, SGXTEX_FILTER_NEAREST);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("blitest: fb0 wrapped at %#x; swap blit submitted and complete in %ld us\n", fb.devva,
		       (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
		return bad;
	}
	if (argc > 2 && !strcmp(argv[2], "rect")) {
		static const struct sgxtex_rect r[][2] = {
			{ { 8, 4, 24, 20 }, { 30, 40, 24, 20 } },
			{ { 40, 10, 16, 16 }, { 8, 24, 16, 16 } },
			{ { 0, 0, 16, 8 }, { 10, 10, 32, 16 } },
		};

		memset(out.cpu, 0x11, out.size);
		memset(tw.cpu, 0x22, tw.size);
		__sync_synchronize();
		blit_srect = &r[0][0], blit_drect = &r[0][1];
		bad |= blit("rsub", &lin, &s_lin, &out, &s_out, SGXTEX_FILTER_NEAREST);
		dump(dir, "rsub", &out, 64 * 64 * 4);
		blit_srect = &r[1][0], blit_drect = &r[1][1];
		bad |= blit("rsubtw", &lin, &s_lin, &tw, &s_tw, SGXTEX_FILTER_NEAREST);
		dump(dir, "rsubtw", &tw, 64 * 64 * 4);
		memset(out.cpu, 0x11, out.size);
		__sync_synchronize();
		blit_srect = &r[2][0], blit_drect = &r[2][1];
		bad |= blit("rscale", &lin, &s_lin, &out, &s_out, SGXTEX_FILTER_NEAREST);
		dump(dir, "rscale", &out, 64 * 64 * 4);
		return bad;
	}
	if (argc > 2 && !strcmp(argv[2], "rect2")) {
		static const struct sgxtex_rect band[][2] = {
			{ { 0, 10, 64, 32 }, { 0, 16, 64, 32 } },
			{ { 0, 10, 64, 16 }, { 0, 20, 64, 16 } },
			{ { 40, 10, 16, 16 }, { 8, 24, 16, 16 } },
		};

		memset(tw.cpu, 0x22, tw.size);
		__sync_synchronize();
		blit_srect = &band[0][0], blit_drect = &band[0][1];
		bad |= blit("rband", &lin, &s_lin, &tw, &s_tw, SGXTEX_FILTER_NEAREST);
		dump(dir, "rband", &tw, 64 * 64 * 4);
		memset(tw.cpu, 0x22, tw.size);
		__sync_synchronize();
		blit_srect = &band[1][0], blit_drect = &band[1][1];
		bad |= blit("rband2", &lin, &s_lin, &tw, &s_tw, SGXTEX_FILTER_NEAREST);
		dump(dir, "rband2", &tw, 64 * 64 * 4);
		memset(tw.cpu, 0x22, tw.size);
		__sync_synchronize();
		blit_srect = blit_drect = NULL;
		bad |= blit("rcomp1", &tw, &s_tw, &out, &s_out, SGXTEX_FILTER_NEAREST);
		blit_srect = &band[2][0], blit_drect = &band[2][1];
		bad |= blit("rcomp2", &lin, &s_lin, &out, &s_out, SGXTEX_FILTER_NEAREST);
		blit_srect = blit_drect = NULL;
		bad |= blit("rcomp3", &out, &s_out, &tw, &s_tw, SGXTEX_FILTER_NEAREST);
		dump(dir, "rcomp", &tw, 64 * 64 * 4);
		return bad;
	}
	if (argc > 2 && !strcmp(argv[2], "sweep")) {
		/* bg[16..22] for 64x64 are 0x1000 0x3f80 0x10400010 1.0 0x104000 0x1000 0x3f80 */
		/* bg[3..8] for any size are 0 0 1.0 0 0 1.0; bg[19] 1.0 */
		static const int v[][4] = {
			{ -1 },
			{ 3, 0x3e800000, -1 },	/* 0.25 */
			{ 4, 0x3e800000, -1 },
			{ 5, 0x3f000000, -1 },	/* 0.5 */
			{ 6, 0x3e800000, -1 },
			{ 7, 0x3e800000, -1 },
			{ 8, 0x3f000000, -1 },
			{ 9, 0x3e800000, -1 },
			{ 19, 0x3f000000, -1 },
		};
		for (unsigned i = 0; i < sizeof v / sizeof *v; i++) {
			char name[16];
			int n = 0;

			for (int j = 0; j + 1 < 4 && v[i][j] >= 0; j += 2, n++)
				bg_override[n][0] = v[i][j], bg_override[n][1] = v[i][j + 1];
			bg_override[n][0] = -1;
			memset(out.cpu, 0, out.size);
			__sync_synchronize();
			snprintf(name, sizeof name, "sweep%u", i);
			bad |= blit(name, &lin, &s_lin, &out, &s_out, SGXTEX_FILTER_NEAREST);
			dump(dir, name, &out, 64 * 64 * 4);
		}
		return bad;
	}

	bad |= blit("up", &lin, &s_lin, &tw, &s_tw, SGXTEX_FILTER_NEAREST);
	dump(dir, "up", &tw, 64 * 64 * 4);
	bad |= blit("down", &tw, &s_tw, &out, &s_out, SGXTEX_FILTER_NEAREST);
	dump(dir, "down", &out, 64 * 64 * 4);
	bad |= blit("mipl", &tw, &s_tw, &mip, &s_mip, SGXTEX_FILTER_LINEAR);
	dump(dir, "mipl", &mip, 32 * 32 * 4);
	bad |= blit("mipn", &tw, &s_tw, &mip, &s_mip, SGXTEX_FILTER_NEAREST);
	dump(dir, "mipn", &mip, 32 * 32 * 4);
	bad |= blit("up2x", &lin, &s_lin, &big, &s_big, SGXTEX_FILTER_NEAREST);
	dump(dir, "up2x", &big, 128 * 128 * 4);
	{
		struct sgxtex_surface d = s_nout;

		d.addr = big.devva;	/* reuse: 104 x 30 x 4 fits */
		bad |= blit("npot", &npot, &s_npot, &big, &d, SGXTEX_FILTER_NEAREST);
		dump(dir, "npot", &big, 104 * 30 * 4);
	}
	bad |= blit("to565", &lin, &s_lin, &o565, &s_565, SGXTEX_FILTER_NEAREST);
	dump(dir, "to565", &o565, 64 * 64 * 2);
	return bad;
}
