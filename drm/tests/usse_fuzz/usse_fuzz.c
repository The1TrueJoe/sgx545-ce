// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Host fuzzer for the USSE instruction check behind UPLOAD_CODE: the
 * kernel's own pvrsgx_usse.c, built against a few shim headers. Each word is
 * checked alone, at instruction 2 of a 512-instruction object placed 0x100
 * instructions into its USSE heap.
 *
 *   usse_fuzz gen SEED N   N words of each generated class
 *   usse_fuzz scan         "hi lo [class]" lines (hex) from stdin
 *
 * Every accepted word is printed as "A class hi lo" for usse_fuzz.py to
 * decode. Counts per class and refusal reason go to stderr as
 * "C class total accepted none form operand kind branch phas".
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../powervr_sgx/pvrsgx_usse.c"

#define FIRST		2
#define OBJ_INSTS	512
#define PLACE		0x100

struct counts {
	const char *name;
	u64 total, accepted, why[PVRSGX_USSE_REFUSAL_PHAS + 1];
};

static u64 rng_state;

/* splitmix64 */
static u64 rnd(void)
{
	u64 z = (rng_state += 0x9e3779b97f4a7c15ull);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

static void check(struct counts *c, u64 w)
{
	enum pvrsgx_usse_refusal why = PVRSGX_USSE_REFUSAL_NONE;
	bool abs = false;

	c->total++;
	if (pvrsgx_usse_scan(PVRSGX_ISA_SGX545, &w, 1, FIRST, OBJ_INSTS, PLACE, &abs, &why) < 0) {
		c->accepted++;
		printf("A %s %08" PRIx32 " %08" PRIx32 "\n", c->name, (u32)(w >> 32), (u32)w);
		return;
	}
	/* A refusal without a reason counts under "none". */
	c->why[why]++;
}

static void report(const struct counts *c)
{
	fprintf(stderr, "C %s %" PRIu64 " %" PRIu64, c->name, c->total, c->accepted);
	for (int i = 0; i <= PVRSGX_USSE_REFUSAL_PHAS; i++)
		fprintf(stderr, " %" PRIu64, c->why[i]);
	fprintf(stderr, "\n");
}

#include "../usse_corpus.h"

static u64 corpus_word(void)
{
	u32 i = rnd() % ARRAY_SIZE(usse_corpus);

	return (u64)usse_corpus[i].hi << 32 | usse_corpus[i].lo;
}

static void gen(u64 seed, u64 n)
{
	struct counts uniform = { .name = "uniform" }, shaped = { .name = "kernel-form" },
		      flip = { .name = "corpus-flip" }, window = { .name = "corpus-window" },
		      floor = { .name = "uapi-corpus" };

	/* The uAPI test's corpus: every word must pass. */
	for (u32 i = 0; i < ARRAY_SIZE(usse_corpus); i++)
		check(&floor, (u64)usse_corpus[i].hi << 32 | usse_corpus[i].lo);

	rng_state = seed;
	for (u64 i = 0; i < n; i++)
		check(&uniform, rnd());

	/* Random bits under each form the kernel knows: the accepted paths. */
	for (u64 i = 0; i < n; i++) {
		const struct pvrsgx_usse_form *f = &pvrsgx_usse_forms[rnd() % ARRAY_SIZE(pvrsgx_usse_forms)];

		check(&shaped, f->match | (rnd() & ~f->mask));
	}

	/* Words the compilers emit, with one to four bits flipped. */
	for (u64 i = 0; i < n; i++) {
		u64 w = corpus_word();
		int k = 1 + rnd() % 4;

		while (k--)
			w ^= 1ull << (rnd() % 64);
		check(&flip, w);
	}

	/* ... or with an aligned byte replaced. */
	for (u64 i = 0; i < n; i++) {
		u64 w = corpus_word();
		int b = 8 * (rnd() % 8);

		w = (w & ~(0xffull << b)) | (rnd() & 0xff) << b;
		check(&window, w);
	}

	report(&floor);
	report(&uniform);
	report(&shaped);
	report(&flip);
	report(&window);
}

#define MAX_CLASSES 256

static void scan(void)
{
	static struct counts classes[MAX_CLASSES];
	static char names[MAX_CLASSES][32];
	char line[256], name[32];
	u32 hi, lo;
	int n = 0;

	while (fgets(line, sizeof(line), stdin)) {
		struct counts *c = NULL;
		int k = sscanf(line, "%" SCNx32 " %" SCNx32 " %31s", &hi, &lo, name);

		if (k < 2)
			continue;
		if (k < 3)
			strcpy(name, "stdin");
		for (int i = 0; i < n; i++) {
			if (!strcmp(names[i], name))
				c = &classes[i];
		}
		if (!c) {
			if (n == MAX_CLASSES) {
				fprintf(stderr, "too many classes\n");
				exit(2);
			}
			strcpy(names[n], name);
			c = &classes[n];
			c->name = names[n++];
		}
		check(c, (u64)hi << 32 | lo);
	}
	for (int i = 0; i < n; i++)
		report(&classes[i]);
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "gen")) {
		gen(strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0));
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "scan")) {
		scan();
		return 0;
	}
	fprintf(stderr, "usage: usse_fuzz gen SEED N | usse_fuzz scan\n");
	return 2;
}
