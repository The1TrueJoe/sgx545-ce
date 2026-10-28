/*
 * replay: run a script made by tools/decode/mkreplay.py through the native pvr
 * client, with no DDK library loaded.
 *
 *   replay [-k] [-H | -W] [-n node] <script> <data>
 *
 * One op per line ('#' starts a comment):
 *   connect | acquire <index> <type> | memctx
 *   alloc <i> <heap id> <attribs> <size> <align> <expected dev addr>
 *   map <i>
 *   wrap <i> <size> <expected dev addr>        (a private page-aligned buffer)
 *   regctx <data off> <size> <PD offset> <captured dev addr>
 *   regxfer <n> <data off> <size> <PD offset> <captured dev addr>   (transfer context n)
 *   zero <dev addr> <len> | write <dev addr> <len> <data off>
 *   poke <dev addr> <word>...                  (32-bit words, consecutive)
 *   sync                                       (memory barrier)
 *   kick key=value ...                         (see mkreplay.py)
 *   xfer ccb=m<i> off=<n> ctx=<n> [tasync=s<i>] [sync3d=s<i>] [src=s<i>,..] [dst=s<i>,..] [flags=<n>]
 *                                              (SGX_SUBMITTRANSFER on transfer context n)
 *   waitsync <s<i>> <ms> | sleep <ms> | dump <dev addr> <len> <file>
 *   flushrt <dev addr>                         (SGX_FLUSH_HW_RENDER_TARGET: drop the RT data set)
 * Buffers are named by capture-order index; m<i> is a buffer's mem info
 * handle, s<i> its sync info handle. A buffer that lands anywhere but its
 * captured device address stops the replay unless -k is given.
 *
 * Device memory and the wrapped buffer are mapped below 4 GB (MAP_32BIT) for
 * drivers that truncate user addresses; -H maps them wherever mmap puts them
 * (above 4 GB on x86_64), to test a driver without that bug; -W does that for
 * the wrapped buffer only. -F wraps the framebuffer (/dev/fb0, mapped) instead of
 * private memory, so a frame the replay presents lands on the display.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pvr.h"

#define MAXBUF 1024

static struct pvr P;
static struct pvr_buf bufs[MAXBUF];
static int nbufs;
static unsigned char *data;
static size_t data_len;
static uint32_t ctx_va, ctx_va_captured;
static int keep_going, wrap_high, wrap_fb;
static uint32_t xctx_va[4];
static uintptr_t cpu_lo = UINTPTR_MAX, cpu_hi;
static int line_no;

#define die(...) do { fprintf(stderr, "replay:%d: ", line_no); fprintf(stderr, __VA_ARGS__); \
		      fputc('\n', stderr); exit(1); } while (0)

static uint32_t num(const char *s)
{
	char *e;
	unsigned long v;

	if (!s)
		die("missing argument");
	v = strtoul(s, &e, 0);
	if (*e)
		die("bad number '%s'", s);
	return (uint32_t)v;
}

static struct pvr_buf *buf(uint32_t i)
{
	if (i >= MAXBUF)
		die("buffer index %u out of range", i);
	if ((int)i >= nbufs)
		nbufs = (int)i + 1;
	return &bufs[i];
}

/* m<i> -> mem info handle, s<i> -> sync info handle, 0 -> NULL */
static IMG_HANDLE handle(const char *s)
{
	struct pvr_buf *b;

	if (!strcmp(s, "0"))
		return NULL;
	if ((s[0] != 'm' && s[0] != 's') || !s[1])
		die("bad handle '%s'", s);
	b = buf(num(s + 1));
	if (!b->mem)
		die("handle '%s': buffer not created", s);
	if (s[0] == 's' && !b->sync)
		die("handle '%s': buffer has no sync info", s);
	return s[0] == 'm' ? b->mem : b->sync;
}

/* CPU address of [va, va+len) inside one mapped buffer */
static char *cpu(uint32_t va, uint32_t len)
{
	for (int i = 0; i < nbufs; i++) {
		struct pvr_buf *b = &bufs[i];

		if (b->cpu && va >= b->devva && va - b->devva < b->size) {
			if (len > b->size - (va - b->devva))
				die("%#x+%#x runs past buffer %d (%#x+%#x)", va, len, i, b->devva, b->size);
			return (char *)b->cpu + (va - b->devva);
		}
	}
	die("no mapped buffer holds %#x", va);
}

static void check_va(const char *what, int i, uint32_t got, uint32_t want)
{
	if (got == want)
		return;
	fprintf(stderr, "replay:%d: %s %d landed at %#x, captured at %#x\n", line_no, what, i, got, want);
	if (!keep_going)
		exit(2);
}

static void bridge(const char *what, int e)
{
	if (e)
		die("%s failed: %d", what, e);
}

static void msleep(uint32_t ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static void op_kick(char **tok, int n)
{
	SGX_CCB_KICK k;
	IMG_HANDLE dst[16];

	memset(&k, 0, sizeof k);
	k.hDevMemContext = P.memctx;
	for (int i = 1; i < n; i++) {
		char *key = tok[i], *v = strchr(key, '=');

		if (v)
			*v++ = 0;
		if (!strcmp(key, "ccb")) {
			k.hCCBKernelMemInfo = handle(v);
		} else if (!strcmp(key, "off")) {
			k.ui32CCBOffset = num(v);
		} else if (!strcmp(key, "cmd")) {
			uint32_t w[8];
			int j = 0;

			for (char *s = strtok(v, ","); s && j < 8; s = strtok(NULL, ","), j++)
				w[j] = strcmp(s, "ctx") ? num(s) : ctx_va;
			if (j != 8)
				die("cmd needs 8 words");
			k.sCommand.ui32ServiceAddress = w[0];
			k.sCommand.ui32CacheControl = w[1];
			memcpy(k.sCommand.ui32Data, w + 2, sizeof k.sCommand.ui32Data);
		} else if (!strcmp(key, "first")) {
			k.bFirstKickOrResume = IMG_TRUE;
		} else if (!strcmp(key, "last")) {
			k.bLastInScene = IMG_TRUE;
		} else if (!strcmp(key, "tadep")) {
			k.bTADependency = IMG_TRUE;
		} else if (!strcmp(key, "hwsync")) {
			k.hKernelHWSyncListMemInfo = handle(v);
		} else if (!strcmp(key, "dst")) {
			for (char *s = strtok(v, ","); s; s = strtok(NULL, ",")) {
				if (k.ui32NumDstSyncObjects >= sizeof dst / sizeof *dst)
					die("too many dst syncs");
				dst[k.ui32NumDstSyncObjects++] = handle(s);
			}
			k.pahDstSyncHandles = dst;
		} else if (!strcmp(key, "ta") || !strcmp(key, "3d")) {
			int ta = key[0] == 't';
			SGX_INTERNEL_STATUS_UPDATE *u;
			char *a = strtok(v, ":"), *b = strtok(NULL, ":"), *c = strtok(NULL, ":");

			if (ta ? k.ui32NumTAStatusVals >= SGX_MAX_TA_STATUS_VALS
			       : k.ui32Num3DStatusVals >= SGX_MAX_3D_STATUS_VALS)
				die("too many status updates");
			u = ta ? &k.asTAStatusUpdate[k.ui32NumTAStatusVals++]
			       : &k.as3DStatusUpdate[k.ui32Num3DStatusVals++];
			u->sCtlStatus.sStatusDevAddr.uiAddr = num(a);
			u->sCtlStatus.ui32StatusValue = num(b);
			u->hKernelMemInfo = handle(c);
		} else if (!strcmp(key, "src")) {
			if (k.ui32NumSrcSyncs >= sizeof k.ahSrcKernelSyncInfo / sizeof *k.ahSrcKernelSyncInfo)
				die("too many src syncs");
			k.ahSrcKernelSyncInfo[k.ui32NumSrcSyncs++] = handle(v);
		} else if (!strcmp(key, "ta3dsync")) {
			k.hTA3DSyncInfo = handle(v);
		} else if (!strcmp(key, "tasync")) {
			k.hTASyncInfo = handle(v);
		} else if (!strcmp(key, "sync3d")) {
			k.h3DSyncInfo = handle(v);
		} else {
			die("unknown kick field '%s'", key);
		}
	}
	bridge("SGX_DOKICK", pvr_kick(&P, &k));
}

/* Poll a sync object's write ops until complete catches up with pending. */
static void op_waitsync(const char *h, uint32_t ms)
{
	struct pvr_buf s = { .mem = handle(h) };
	volatile PVRSRV_SYNC_DATA *d;
	uint32_t t = 0;

	bridge("MHANDLE_TO_MMAP_DATA (sync)", pvr_map(&P, &s));
	d = s.cpu;
	while (d->ui32WriteOpsComplete < d->ui32WriteOpsPending && t < ms) {
		msleep(1);
		t++;
	}
	printf("replay: %s write ops %u/%u, read ops %u/%u after %u ms%s\n", h, d->ui32WriteOpsComplete,
	       d->ui32WriteOpsPending, d->ui32ReadOpsComplete, d->ui32ReadOpsPending, t,
	       t >= ms ? " (timed out)" : "");
}

static void op_xfer(char **tok, int n)
{
	PVRSRV_TRANSFER_SGX_KICK k;

	memset(&k, 0, sizeof k);
	for (int i = 1; i < n; i++) {
		char *key = tok[i], *v = strchr(key, '=');

		if (v)
			*v++ = 0;
		if (!v)
			die("xfer field '%s' needs a value", key);
		if (!strcmp(key, "ccb")) {
			k.hCCBMemInfo = handle(v);
		} else if (!strcmp(key, "off")) {
			k.ui32SharedCmdCCBOffset = num(v);
		} else if (!strcmp(key, "ctx")) {
			if (num(v) >= 4 || !xctx_va[num(v)])
				die("transfer context %s not registered", v);
			k.sHWTransferContextDevVAddr.uiAddr = xctx_va[num(v)];
		} else if (!strcmp(key, "tasync")) {
			k.hTASyncInfo = handle(v);
		} else if (!strcmp(key, "sync3d")) {
			k.h3DSyncInfo = handle(v);
		} else if (!strcmp(key, "src") || !strcmp(key, "dst")) {
			int src = key[0] == 's';

			for (char *s = strtok(v, ","); s; s = strtok(NULL, ",")) {
				IMG_UINT32 *cnt = src ? &k.ui32NumSrcSync : &k.ui32NumDstSync;

				if (*cnt >= SGX_MAX_TRANSFER_SYNC_OPS)
					die("too many transfer syncs");
				(src ? k.ahSrcSyncInfo : k.ahDstSyncInfo)[(*cnt)++] = handle(s);
			}
		} else if (!strcmp(key, "flags")) {
			k.ui32Flags = num(v);
		} else {
			die("unknown xfer field '%s'", key);
		}
	}
	bridge("SGX_SUBMITTRANSFER", pvr_submit_transfer(&P, &k));
}

static void run(FILE *f)
{
	char line[8192];
	int nallocs = 0, nkicks = 0;

	while (fgets(line, sizeof line, f)) {
		char *tok[64];
		int n = 0;

		line_no++;
		for (char *s = strtok(line, " \t\r\n"); s && n < 64; s = strtok(NULL, " \t\r\n"))
			tok[n++] = s;
		if (!n || tok[0][0] == '#')
			continue;
		if (!strcmp(tok[0], "connect")) {
			bridge("CONNECT_SERVICES", pvr_connect(&P));
		} else if (!strcmp(tok[0], "acquire")) {
			bridge("ACQUIRE_DEVICEINFO", pvr_acquire(&P, num(tok[1]), num(tok[2])));
		} else if (!strcmp(tok[0], "memctx")) {
			bridge("CREATE_DEVMEMCONTEXT", pvr_memctx(&P));
		} else if (!strcmp(tok[0], "alloc")) {
			uint32_t i = num(tok[1]);
			struct pvr_buf *b = buf(i);
			int e = pvr_alloc(&P, b, num(tok[2]), num(tok[3]), num(tok[4]), num(tok[5]));

			if (e)
				die("ALLOC_DEVICEMEM %u (heap %s size %s) failed: %d", i, tok[2], tok[4], e);
			check_va("buffer", (int)i, b->devva, num(tok[6]));
			nallocs++;
		} else if (!strcmp(tok[0], "map")) {
			uint32_t i = num(tok[1]);

			if (pvr_map(&P, buf(i)))
				die("map of buffer %u failed", i);
			if ((uintptr_t)bufs[i].cpu < cpu_lo)
				cpu_lo = (uintptr_t)bufs[i].cpu;
			if ((uintptr_t)bufs[i].cpu > cpu_hi)
				cpu_hi = (uintptr_t)bufs[i].cpu;
		} else if (!strcmp(tok[0], "wrap")) {
			uint32_t i = num(tok[1]), size = num(tok[2]);
			struct pvr_buf *b = buf(i);
			int fb = wrap_fb ? open("/dev/fb0", O_RDWR) : -1;
			void *m = fb >= 0 ? mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | (wrap_high ? 0 : P.map_flags), fb, 0)
					  : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (wrap_high ? 0 : P.map_flags), -1, 0);

			if (wrap_fb && fb < 0)
				die("open /dev/fb0: %s", strerror(errno));
			if (m == MAP_FAILED)
				die("mmap for wrap: %s", strerror(errno));
			if (fb < 0)
				memset(m, 0, size);	/* fault the pages in */
			bridge("WRAP_EXT_MEMORY", pvr_wrap(&P, b, m, size));
			check_va("wrapped buffer", (int)i, b->devva, num(tok[3]));
			printf("replay: wrapped %#x bytes at %p\n", size, m);
		} else if (!strcmp(tok[0], "regctx")) {
			uint32_t off = num(tok[1]), size = num(tok[2]);
			IMG_HANDLE h;

			if ((size_t)off + size > data_len)
				die("regctx data out of range");
			ctx_va_captured = num(tok[4]);
			bridge("SGX_REGISTER_HW_RENDER_CONTEXT",
			       pvr_register_render_ctx(&P, data + off, size, num(tok[3]), &h, &ctx_va));
			if (ctx_va != ctx_va_captured)
				fprintf(stderr, "replay: render context at %#x, captured at %#x (kicks use %#x)\n",
					ctx_va, ctx_va_captured, ctx_va);
		} else if (!strcmp(tok[0], "regxfer")) {
			uint32_t x = num(tok[1]), off = num(tok[2]), size = num(tok[3]);
			IMG_HANDLE h;

			if (x >= 4 || (size_t)off + size > data_len)
				die("bad regxfer");
			bridge("SGX_REGISTER_HW_TRANSFER_CONTEXT",
			       pvr_register_transfer_ctx(&P, data + off, size, num(tok[4]), &h, &xctx_va[x]));
			if (xctx_va[x] != num(tok[5]))
				fprintf(stderr, "replay: transfer context %u at %#x, captured at %s\n", x, xctx_va[x], tok[5]);
		} else if (!strcmp(tok[0], "zero")) {
			uint32_t len = num(tok[2]);

			memset(cpu(num(tok[1]), len), 0, len);
		} else if (!strcmp(tok[0], "write")) {
			uint32_t va = num(tok[1]), len = num(tok[2]), off = num(tok[3]);
			char *dst = cpu(va, len);

			if ((size_t)off + len > data_len)
				die("write data out of range");
			memcpy(dst, data + off, len);
			/* words holding the captured render context address follow it */
			if (ctx_va != ctx_va_captured)
				for (uint32_t o = 0; o + 4 <= len; o += 4)
					if (!memcmp(dst + o, &ctx_va_captured, 4))
						memcpy(dst + o, &ctx_va, 4);
		} else if (!strcmp(tok[0], "poke")) {
			uint32_t va = num(tok[1]);

			if (n < 3)
				die("poke needs a word");
			for (int j = 2; j < n; j++) {
				uint32_t w = num(tok[j]);

				memcpy(cpu(va + 4 * (uint32_t)(j - 2), 4), &w, 4);
			}
		} else if (!strcmp(tok[0], "sync")) {
			__sync_synchronize();
		} else if (!strcmp(tok[0], "kick")) {
			nkicks++;
			op_kick(tok, n);
			printf("replay: kick %d sent\n", nkicks);
		} else if (!strcmp(tok[0], "xfer")) {
			op_xfer(tok, n);
			printf("replay: transfer sent\n");
		} else if (!strcmp(tok[0], "waitsync")) {
			op_waitsync(tok[1], num(tok[2]));
		} else if (!strcmp(tok[0], "flushrt")) {
			bridge("SGX_FLUSH_HW_RENDER_TARGET", pvr_flush_rt(&P, num(tok[1])));
		} else if (!strcmp(tok[0], "sleep")) {
			msleep(num(tok[1]));
		} else if (!strcmp(tok[0], "dump")) {
			uint32_t len = num(tok[2]);
			FILE *o = fopen(tok[3], "wb");

			if (!o || fwrite(cpu(num(tok[1]), len), 1, len, o) != len || fclose(o))
				die("dump to %s failed", tok[3]);
			printf("replay: dumped %#x bytes at %s to %s\n", len, tok[1], tok[3]);
		} else {
			die("unknown op '%s'", tok[0]);
		}
	}
	printf("replay: %d buffers allocated, %d kicks, device memory mapped at %#lx..%#lx\n", nallocs, nkicks,
	       (unsigned long)cpu_lo, (unsigned long)cpu_hi);
}

int main(int argc, char **argv)
{
	const char *node = NULL;
	FILE *f, *d;
	int c, high = 0;

	while ((c = getopt(argc, argv, "kHWFn:")) != -1) {
		if (c == 'k')
			keep_going = 1;
		else if (c == 'F')
			wrap_fb = 1;
		else if (c == 'H')
			high = wrap_high = 1;
		else if (c == 'W')
			wrap_high = 1;
		else if (c == 'n')
			node = optarg;
		else
			return 1;
	}
	if (argc - optind != 2) {
		fprintf(stderr, "usage: replay [-k] [-H | -W] [-F] [-n node] <script> <data>\n");
		return 1;
	}
	if (!(f = fopen(argv[optind], "r")) || !(d = fopen(argv[optind + 1], "rb"))) {
		perror("replay");
		return 1;
	}
	fseek(d, 0, SEEK_END);
	data_len = (size_t)ftell(d);
	rewind(d);
	if (!(data = malloc(data_len + 1)) || fread(data, 1, data_len, d) != data_len) {
		perror("replay: data");
		return 1;
	}
	fclose(d);
	if (pvr_open(&P, node))
		die("open %s: %s", node ? node : "/dev/dri/card0", strerror(errno));
	if (high)
		P.map_flags = 0;
	run(f);
	return 0;
}
