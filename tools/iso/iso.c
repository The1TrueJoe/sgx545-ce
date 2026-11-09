/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */
/*
 * iso: run one isolation test on the SGX545 (docs/UAPI.md section 7.2): a frame from
 * tools/gen/sgxgen.c whose fragment and vertex programs, code placement and DOUTU words
 * come from a spec file, as an ordinary services client. tools/iso/iso.py writes the
 * specs and reads the results.
 *
 *   iso spec
 *
 * Spec, one directive per line (numbers hex, REF = a number or NAME+off):
 *   buf NAME HEAP ATTR SIZE [ALIGN]   allocate and map a buffer; a failure is reported, not fatal
 *   fill NAME OFF WORD...             host writes it before the frame
 *   fs WORD...                        fragment code, low word first (default: the uniform-colour shader)
 *   fsbuf NAME OFF                    copy the fragment code into that buffer instead of the shader heap
 *   fsw0 BASE CODEBASE ORBITS         DOUTU w0 = base | (va - codebase)/8 << 4 | temps | ORBITS
 *   fstemps N                         fragment temporaries
 *   fsa N REF                         word N (0-8) of the fragment secondary block: sa<N>
 *   vs WORD... ; vsreloc INDEX... ; vstemps N ; vsa N REF (sa<6+N>)   the same for the vertex program
 *   reg OFF                           read this named register from the host around the frame
 *   dump NAME OFF COUNT               print these words after the frame
 * Output: "va NAME x", "reg OFF before after", "dump NAME OFF words...",
 * "pix WORD uniform=N bg=WORD", "render done|TIMEOUT".
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sgxgen.h"

#define MAXW 2048
#define W 720
#define H 480

struct nbuf {
	char name[16];
	struct sgx_bo bo;
	int ok;
};
static struct nbuf bufs[16];
static unsigned nbufs;

static struct nbuf *find(const char *n)
{
	for (unsigned i = 0; i < nbufs; i++)
		if (!strcmp(bufs[i].name, n))
			return &bufs[i];
	return NULL;
}

static int ref(const char *s, uint32_t *v)
{
	char tmp[32], *plus;
	struct nbuf *b;

	if (s[0] >= '0' && s[0] <= '9') {
		*v = (uint32_t)strtoul(s, NULL, 16);
		return 0;
	}
	snprintf(tmp, sizeof tmp, "%s", s);
	plus = strchr(tmp, '+');
	if (plus)
		*plus++ = 0;
	b = find(tmp);
	if (!b || !b->ok)
		return -1;
	*v = b->bo.va + (plus ? (uint32_t)strtoul(plus, NULL, 16) : 0);
	return 0;
}

/* the register block is at the start of the SGX function's BAR 0 */
static volatile uint32_t *regs_map(void)
{
	int fd = open("/sys/bus/pci/devices/0000:01:02.0/resource0", O_RDWR | O_SYNC);
	void *m;

	if (fd < 0)
		return NULL;
	m = mmap(NULL, 0x4000, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	return m == MAP_FAILED ? NULL : m;
}

int main(int argc, char **argv)
{
	static uint32_t fsw[MAXW], vsw[MAXW], fsa[9], vsa[4];
	static char *lines[512];
	static unsigned regl[32], reloc[16];
	static uint32_t regb[32];
	unsigned nfs = 0, nvs = 0, nreg = 0, nreloc = 0, nl = 0, fsa_n = 0, vsa_n = 0;
	uint32_t fstemps = 0, vstemps = 0;
	struct nbuf *fsbuf = NULL;
	uint32_t fsbuf_off = 0, base = 0, codebase = 0, orbits = 0;
	int fsw0 = 0;
	struct sgx_dev dev;
	struct sgx_ctx *c;
	struct sgx_surface *s;
	struct sgx_shader *vs, *fs;
	struct sgx_state st = { .front = SGX_CCW };
	float clear[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
	float tri[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f };
	char line[8192];
	volatile uint32_t *regs;
	FILE *f;
	int e;

	if (argc != 2 || !(f = fopen(argv[1], "r")))
		return fprintf(stderr, "usage: iso spec\n"), 1;
	while (fgets(line, sizeof line, f) && nl < 512)
		lines[nl++] = strdup(line);
	fclose(f);
	if ((e = sgx_dev_open(&dev, NULL, NULL)))
		return fprintf(stderr, "iso: services: %d\n", e), 1;
	c = sgx_ctx_create(&dev);
	s = sgx_surface_create(c, W, H, 0);
	vs = sgx_shader_builtin(c, SGX_VS_XY);
	fs = sgx_shader_builtin(c, SGX_FS_UNIFORM);
	regs = regs_map();

	for (unsigned l = 0; l < nl; l++) {
		char *t = strtok(lines[l], " \t\n");

		if (!t)
			continue;
		if (!strcmp(t, "buf")) {
			struct nbuf *b = &bufs[nbufs++];
			uint32_t hp, at, sz, al = 0x1000;
			char *x;

			snprintf(b->name, sizeof b->name, "%s", strtok(NULL, " \t\n"));
			hp = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
			at = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
			sz = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
			if ((x = strtok(NULL, " \t\n")))
				al = (uint32_t)strtoul(x, NULL, 16);
			e = pvr_alloc(&dev.pvr, &b->bo.b, hp, at, sz, al);
			if (!e)
				e = pvr_map(&dev.pvr, &b->bo.b);
			if (e) {
				printf("va %s FAILED %d\n", b->name, e);
				continue;
			}
			b->bo.va = b->bo.b.devva;
			b->bo.map = b->bo.b.cpu;
			memset(b->bo.map, 0, sz);
			b->ok = 1;
			printf("va %s %#x\n", b->name, b->bo.va);
		} else if (!strcmp(t, "fill")) {
			struct nbuf *b = find(strtok(NULL, " \t\n"));
			uint32_t o = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
			char *x;

			while ((x = strtok(NULL, " \t\n")))
				if (b && b->ok) {
					b->bo.map[o / 4] = (uint32_t)strtoul(x, NULL, 16);
					o += 4;
				}
		} else if (!strcmp(t, "fs") || !strcmp(t, "vs")) {
			uint32_t *w = t[0] == 'f' ? fsw : vsw;
			unsigned *n = t[0] == 'f' ? &nfs : &nvs;
			char *x;

			while ((x = strtok(NULL, " \t\n")) && *n < MAXW)
				w[(*n)++] = (uint32_t)strtoul(x, NULL, 16);
		} else if (!strcmp(t, "fsbuf")) {
			fsbuf = find(strtok(NULL, " \t\n"));
			fsbuf_off = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
		} else if (!strcmp(t, "fsw0")) {
			base = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
			codebase = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
			orbits = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
			fsw0 = 1;
		} else if (!strcmp(t, "fstemps")) {
			fstemps = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
		} else if (!strcmp(t, "vstemps")) {
			vstemps = (uint32_t)strtoul(strtok(NULL, " \t\n"), NULL, 16);
		} else if (!strcmp(t, "vsreloc")) {
			char *x;

			while ((x = strtok(NULL, " \t\n")) && nreloc < 16)
				reloc[nreloc++] = (unsigned)strtoul(x, NULL, 16);
		} else if (!strcmp(t, "fsa") || !strcmp(t, "vsa")) {
			unsigned i = (unsigned)strtoul(strtok(NULL, " \t\n"), NULL, 16);
			uint32_t v;

			if (ref(strtok(NULL, " \t\n"), &v))
				return printf("ref FAILED\n"), 3;
			if (t[0] == 'f' && i < 9) {
				fsa[i] = v;
				fsa_n = i + 1 > fsa_n ? i + 1 : fsa_n;
			} else if (t[0] == 'v' && i < 4) {
				vsa[i] = v;
				vsa_n = i + 1 > vsa_n ? i + 1 : vsa_n;
			}
		} else if (!strcmp(t, "reg")) {
			if (nreg < 32)
				regl[nreg++] = (unsigned)strtoul(strtok(NULL, " \t\n"), NULL, 16);
		} else if (strcmp(t, "dump") && t[0] != '#') {
			return fprintf(stderr, "iso: bad line: %s\n", t), 1;
		}
	}
	if (nfs) {
		uint32_t va;

		if (fsbuf) {
			if (!fsbuf->ok)
				return printf("fsbuf FAILED\n"), 3;
			memcpy(fsbuf->bo.map + fsbuf_off / 4, fsw, nfs * 4);
			va = fsbuf->bo.va + fsbuf_off;
		} else {
			va = sgx_usse_load(c, 0, fsw, nfs / 2, NULL, 0);
		}
		fs->code = va;
		fs->temps = fstemps;
		if (fsw0)
			fs->w0 = base | ((va - codebase) >> 3) << 4 | ((fstemps + 3) >> 2) << 24 | orbits;
	}
	fs->sec_init = fsa;
	fs->sec_init_n = fsa_n;
	if (nvs) {
		vs->code = sgx_usse_load(c, 1, vsw, nvs / 2, reloc, nreloc);
		vs->temps = vstemps;
	}
	vs->sec_init = vsa;
	vs->sec_init_n = vsa_n;
	for (unsigned i = 0; regs && i < nreg; i++)
		regb[i] = regs[regl[i] / 4];

	sgx_set_framebuffer(c, s);
	sgx_clear(c, clear, 1.0f);
	{
		struct sgx_draw d = { .prim = SGX_TRIANGLES, .vs = vs, .fs = fs, .state = &st, .count = 3,
				      .verts = tri, .color = { 0, 1, 0, 1 } };

		sgx_draw(c, &d);
	}
	fflush(stdout);
	e = sgx_flush(c, 3000);
	for (unsigned i = 0; i < nreg; i++)
		printf("reg %#x %#x %#x\n", regl[i], regs ? regb[i] : 0, regs ? regs[regl[i] / 4] : 0);
	printf("render %s\n", e ? "TIMEOUT" : "done");
	{
		uint32_t *row = s->color.map + 279u * s->stride + 344;
		int uni = 1;

		for (int i = 1; i < 32; i++)
			uni &= row[i] == row[0];
		printf("pix %#x uniform=%d bg=%#x\n", row[0], uni, s->color.map[0]);
	}
	/* dumps come after the render, in spec order; strtok has eaten the lines, so re-read */
	f = fopen(argv[1], "r");
	while (f && fgets(line, sizeof line, f)) {
		char nm[16];
		unsigned o, n;
		struct nbuf *b;

		if (sscanf(line, "dump %15s %x %x", nm, &o, &n) != 3 || !(b = find(nm)) || !b->ok)
			continue;
		printf("dump %s %#x", nm, o);
		for (unsigned i = 0; i < n; i++)
			printf(" %08x", b->bo.map[o / 4 + i]);
		printf("\n");
	}
	if (f)
		fclose(f);
	fflush(stdout);
	sgx_surface_destroy(c, s);
	sgx_ctx_destroy(c);
	sgx_dev_close(&dev);
	return e ? 2 : 0;
}
