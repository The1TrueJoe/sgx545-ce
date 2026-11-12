/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

/*
 * The SGX545 records of pvrsgx_equiv_test.c: each phase's line count and
 * CRC32. Change these only for a change meant to change the SGX545's behavior, and say so in the
 * commit message.
 */

#ifndef PVRSGX_EQUIV_GOLDEN_H
#define PVRSGX_EQUIV_GOLDEN_H

struct pvrsgx_equiv_golden {
	const char *phase;
	u32 lines;
	u32 crc;
};

static const struct pvrsgx_equiv_golden pvrsgx_equiv_golden_545[] = {
	{ "init", 102, 0x0df03815 },
	{ "start", 82, 0xb260b7dd },
	{ "send", 20, 0x09fa89bc },
	{ "recover", 197, 0x4ae5dc12 },
	{ "stop", 26, 0x936dee2f },
	{ "resume", 82, 0xb260b7dd },
	{ "ctx", 29, 0xeb46322d },
	{ "render", 429, 0xe7477b44 },
	{ "heaps", 22, 0x85d1b2f7 },
	{ "teardown", 0, 0x00000000 },
};

#endif /* PVRSGX_EQUIV_GOLDEN_H */
