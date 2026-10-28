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
 */
#include <errno.h>
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

static int blit(const char *name, struct pvr_buf *sb, const struct sgxtex_surface *src, struct pvr_buf *db,
		const struct sgxtex_surface *dst, int filter)
{
	struct sgxtex_blit b;
	uint32_t cmd[SGXTEX_CMD_BYTES / 4];
	PVRSRV_TRANSFER_SGX_KICK k;
	volatile PVRSRV_SYNC_DATA *sd = sync_data(db->sync);
	unsigned t = 0;
	int e;

	sgxtex_blit_build(&b, &X, src, dst, filter);
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
