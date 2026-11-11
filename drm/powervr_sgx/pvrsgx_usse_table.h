/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * The USSE forms, their operand slots and what the validator does with
 * each, from the isaspec description of the instruction set Mesa's
 * compiler uses (src/sgx/isa/sgx.xml). Each form is the bits its
 * patterns fix; each slot the fields its operand is built from.
 */

static const struct pvrsgx_usse_slot pvrsgx_usse_slots[][USSE_MAX_SLOTS] = {
	[0] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[1] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[2] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
	},
	[3] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 } } },
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[4] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 } } },
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 }, { 38, 1, 8 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 39, 1, 9 } } },
	},
	[5] = {
		{ USSE_SLOT_DST, true, 20, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[6] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 14, 7, 0 }, { 30, 2, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[7] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[8] = {
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 14, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[9] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 14, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[10] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 }, { 47, 1, 8 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[11] = {
		{ USSE_SLOT_DST1, false, USSE_ALWAYS, { { 21, 7, 0 }, { 39, 1, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[12] = {
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 }, { 50, 1, 8 } } },
		{ USSE_SLOT_DST1, false, USSE_ALWAYS, { { 21, 7, 0 }, { 39, 1, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[13] = {
		{ USSE_SLOT_DST1, false, USSE_ALWAYS, { { 21, 7, 0 }, { 36, 1, 7 } } },
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 } } },
	},
	[14] = {
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 }, { 50, 1, 8 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[15] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 } } },
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 }, { 51, 1, 8 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[16] = { },
	[17] = {
		{ USSE_SLOT_SRC0, true, USSE_ALWAYS, { { 14, 7, 0 }, { 34, 1, 7 }, { 51, 1, 8 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[18] = {
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
		{ USSE_SLOT_DST1, false, USSE_ALWAYS, { { 21, 7, 0 }, { 39, 1, 7 } } },
	},
	[19] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 }, { 51, 1, 9 } } },
	},
	[20] = {
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
	},
	[21] = {
		{ USSE_SLOT_DST, true, USSE_ALWAYS, { { 21, 7, 0 }, { 32, 2, 7 } } },
	},
	[22] = {
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
	},
	[23] = {
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 0, 7, 0 }, { 28, 2, 7 }, { 48, 1, 9 } } },
		{ USSE_SLOT_SRC, true, USSE_ALWAYS, { { 7, 7, 0 }, { 30, 2, 7 }, { 49, 1, 9 } } },
	},
};

/* 109 forms, 126 entries */
static const struct pvrsgx_usse_form pvrsgx_usse_forms[] = {
	{ 0xf800060000000000ull, 0x0000020000000000ull, USSE_OK, 0 },	/* fadm */
	{ 0xf800060000000000ull, 0x0000000000000000ull, USSE_OK, 0 },	/* fmad */
	{ 0xf800060000000000ull, 0x0000040000000000ull, USSE_OK, 0 },	/* fmsa */
	{ 0xf8000684001fc000ull, 0x0000060000000000ull, USSE_OK, 1 },	/* fsubflr */
	{ 0xf8410600301fc07full, 0x0801040000000000ull, USSE_OK, 2 },	/* fcos */
	{ 0xf8410600301fc07full, 0x0800060000000000ull, USSE_OK, 2 },	/* fexp2 */
	{ 0xf8410600301fc07full, 0x0800040000000000ull, USSE_OK, 2 },	/* flog2 */
	{ 0xf8410600301fc07full, 0x0800000000000000ull, USSE_OK, 2 },	/* frcp */
	{ 0xf8410600301fc07full, 0x0800020000000000ull, USSE_OK, 2 },	/* frsq */
	{ 0xf8410600301fc07full, 0x0801020000000000ull, USSE_OK, 2 },	/* fsin */
	{ 0xf8410600301fc07full, 0x0801000000000000ull, USSE_OK, 2 },	/* fsqrt */
	{ 0xf800060000000000ull, 0x1000020000000000ull, USSE_OK, 3 },	/* fddp */
	{ 0xf800060000000000ull, 0x1000040000000000ull, USSE_OK, 3 },	/* fddpc */
	{ 0xf8000684001fc000ull, 0x1000000000000000ull, USSE_OK, 1 },	/* fdp */
	{ 0xf8000684001c4000ull, 0x1000000000004000ull, USSE_OK, 1 },	/* fdpc */
	{ 0xf800060000000000ull, 0x1800000000000000ull, USSE_OK, 0 },	/* fmaxmin */
	{ 0xf800060000000000ull, 0x1800020000000000ull, USSE_OK, 0 },	/* fminmax */
	{ 0xf8000684001fc000ull, 0x2000000000000000ull, USSE_OK, 1 },	/* fdsx */
	{ 0xf8000684001fc000ull, 0x2000020000000000ull, USSE_OK, 1 },	/* fdsy */
	{ 0xf84107fc301fc07full, 0x2801000020000000ull, USSE_OK, 2 },	/* mov */
	{ 0xf800077800000000ull, 0x2800050000000000ull, USSE_OK, 0 },	/* movc.c10 */
	{ 0xf800077800000000ull, 0x2800040000000000ull, USSE_OK, 0 },	/* movc.f32 */
	{ 0xf800077800000000ull, 0x2800020000000000ull, USSE_OK, 0 },	/* movc.i16 */
	{ 0xf800077800000000ull, 0x2800030000000000ull, USSE_OK, 0 },	/* movc.i32 */
	{ 0xf800077800000000ull, 0x2800010000000000ull, USSE_OK, 0 },	/* movc.u8 */
	{ 0xf800071c001fc000ull, 0x2800060000000000ull, USSE_REFUSE, 1 },	/* op0xf3.s6 */
	{ 0xf800071800000000ull, 0x2800070000000000ull, USSE_REFUSE, 0 },	/* op0xf3.s7 */
	{ 0xf800000000000000ull, 0x3000000000000000ull, USSE_OK, 0 },	/* fmad16 */
	{ 0xf800000000000000ull, 0x3800000000000000ull, USSE_OK, 4 },	/* efo */
	{ 0xf800000000000000ull, 0x4000000000000000ull, USSE_OK, 1 },	/* pck */
	{ 0xf800000000000000ull, 0x4800000000000000ull, USSE_OK, 5 },	/* test */
	{ 0xf800000800000000ull, 0x5000000000000000ull, USSE_OK, 1 },	/* and */
	{ 0xf800000800000000ull, 0x5000000800000000ull, USSE_OK, 1 },	/* or */
	{ 0xf800000800000000ull, 0x5800000000000000ull, USSE_OK, 1 },	/* xor */
	{ 0xf800000800000000ull, 0x6000000800000000ull, USSE_OK, 1 },	/* rol */
	{ 0xf800000800000000ull, 0x6000000000000000ull, USSE_OK, 1 },	/* shl */
	{ 0xf800000800000000ull, 0x6800000800000000ull, USSE_OK, 1 },	/* asr */
	{ 0xf800000800000000ull, 0x6800000000000000ull, USSE_OK, 1 },	/* shr */
	{ 0xf8010ff8301f007full, 0x7001000020000000ull, USSE_OK, 2 },	/* rlp */
	{ 0xf800004c00000000ull, 0x7800000000000000ull, USSE_OK, 5 },	/* testmask */
	{ 0xf800000000000000ull, 0x8000000000000000ull, USSE_OK, 1 },	/* sop2 */
	{ 0xf820040000000000ull, 0x8820000000000000ull, USSE_OK, 0 },	/* lrp1 */
	{ 0xf820040000000000ull, 0x8800000000000000ull, USSE_OK, 0 },	/* sop3 */
	{ 0xf820040000000000ull, 0x8800040000000000ull, USSE_OK, 0 },	/* sop3l */
	{ 0xf820040000000000ull, 0x8820040000000000ull, USSE_OK, 0 },	/* sop3x */
	{ 0xf8000004001fc000ull, 0x9000000000000000ull, USSE_OK, 1 },	/* sopwm */
	{ 0xf800000000000000ull, 0x9800000000000000ull, USSE_OK, 0 },	/* ima8 */
	{ 0xf800800000000000ull, 0xa000000000000000ull, USSE_OK, 0 },	/* ima16 */
	{ 0xf800000000000000ull, 0xa800000000000000ull, USSE_OK, 0 },	/* imae */
	{ 0xf9308ffc001fc000ull, 0xb000000000000000ull, USSE_OK, 1 },	/* adif */
	{ 0xf9300ff800000000ull, 0xb000004000000000ull, USSE_OK, 0 },	/* adifsum */
	{ 0xf83000f800000000ull, 0xb020000000000000ull, USSE_OK, 0 },	/* bilin */
	{ 0xf930400000000000ull, 0xb010000000000000ull, USSE_REFUSE, 0 },	/* op16s1 */
	{ 0xf8320b7c00000000ull, 0xb030000000000000ull, USSE_OK, 6 },	/* ssum16 */
	{ 0xf8320b7c00000000ull, 0xb030020000000000ull, USSE_OK, 6 },	/* ssum16.o8 */
	{ 0xf8320b7c00000000ull, 0xb030010000000000ull, USSE_OK, 6 },	/* ssum16.s8 */
	{ 0xf8320b7cc0003f80ull, 0xb030030000000000ull, USSE_REFUSE, 7 },	/* ssum16i */
	{ 0xf800800400000000ull, 0xb800000400000000ull, USSE_REFUSE, 8 },	/* firh */
	{ 0xf800000400000000ull, 0xb800000000000000ull, USSE_OK, 9 },	/* firv */
	{ 0xf9000020001c4000ull, 0xc000000000000000ull, USSE_OK, 1 },	/* dot3 */
	{ 0xf900002000000000ull, 0xc000002000000000ull, USSE_OK, 0 },	/* dot3s */
	{ 0xf9000020001c4000ull, 0xc100000000000000ull, USSE_OK, 1 },	/* dot4 */
	{ 0xf900002000000000ull, 0xc100002000000000ull, USSE_OK, 0 },	/* dot4s */
	{ 0xf800000000000000ull, 0xc800000000000000ull, USSE_OK, 0 },	/* fpma */
	{ 0xf930000800000000ull, 0xd000000000000000ull, USSE_OK, 10 },	/* ima32 */
	{ 0xf9388f7e001fc000ull, 0xd010000000000000ull, USSE_REFUSE, 11 },	/* op1adrc */
	{ 0xf800000000000000ull, 0xd800000000000000ull, USSE_REFUSE, 3 },	/* fdual */
	{ 0xf808000200000000ull, 0xe000000000000000ull, USSE_OK, 12 },	/* smp */
	{ 0xf840000800000000ull, 0xe840000000000000ull, USSE_OK, 13 },	/* eld.d */
	{ 0xf840000800000000ull, 0xe840000800000000ull, USSE_OK, 13 },	/* eld.q */
	{ 0xf840000000000000ull, 0xe800000000000000ull, USSE_OK, 12 },	/* ld */
	{ 0xf80000800fe00000ull, 0xf000000000000000ull, USSE_OK, 14 },	/* st */
	{ 0xff30f00000000000ull, 0xf930000000000000ull, USSE_OK, 15 },	/* atst8 */
	{ 0xf87fe1fffffc0000ull, 0xf800000000000000ull, USSE_REFUSE, 16 },	/* ba */
	{ 0xff7fe3fffffc0000ull, 0xf800004000000000ull, USSE_BR, 16 },	/* br */
	{ 0xff7fe3fffffc0000ull, 0xf900004000000000ull, USSE_BR, 16 },	/* br */
	{ 0xff7fe3fffffc0000ull, 0xfa00004000000000ull, USSE_BR, 16 },	/* br */
	{ 0xff7fe3fffffc0000ull, 0xfb00004000000000ull, USSE_BR, 16 },	/* br */
	{ 0xff7fe3fffffc0000ull, 0xfc00004000000000ull, USSE_BR, 16 },	/* br */
	{ 0xff7fe3fffffc0000ull, 0xfd00004000000000ull, USSE_BR, 16 },	/* br */
	{ 0xff7fe3fffffc0000ull, 0xfe00004000000000ull, USSE_BR, 16 },	/* br */
	{ 0xff70f1cb0fe00000ull, 0xfb30000000000000ull, USSE_OK, 17 },	/* depthf */
	{ 0xff30000000000000ull, 0xfb20000000000000ull, USSE_OK, 17 },	/* emit */
	{ 0xffffb7feffffffffull, 0xf820000000000000ull, USSE_OK, 16 },	/* idf */
	{ 0xfffb000000000000ull, 0xfc10000000000000ull, USSE_REFUSE, 16 },	/* imo */
	{ 0xf87fe7ffffffffffull, 0xf800008000000000ull, USSE_REFUSE, 16 },	/* lapc */
	{ 0xff7ef77ec0003f80ull, 0xfe20000000000000ull, USSE_REFUSE, 18 },	/* ldr */
	{ 0xff30000cf0000000ull, 0xfc20000000000000ull, USSE_OK, 19 },	/* limm */
	{ 0xfffff7ffffffff8full, 0xfd20000000000000ull, USSE_REFUSE, 16 },	/* lock */
	{ 0xff7bf7fffffffffcull, 0xf800014000000000ull, USSE_OK, 16 },	/* nop */
	{ 0xfffc34ff0fffc000ull, 0xf840300000000000ull, USSE_REFUSE, 20 },	/* op0x100 */
	{ 0xff7fe7ffffffffffull, 0xf808020000000000ull, USSE_REFUSE, 16 },	/* op0x98 */
	{ 0xfffff7ffffffffffull, 0xff20000000000000ull, USSE_OK, 16 },	/* op0xd6 */
	{ 0xfffff7fffffffff0ull, 0xfa20000000000000ull, USSE_REFUSE, 16 },	/* op0xd7 */
	{ 0xff7387fcf01fffffull, 0xfe10000000000000ull, USSE_REFUSE, 19 },	/* op0xfd */
	{ 0xfffc34ff0fffc000ull, 0xf840200000000000ull, USSE_REFUSE, 20 },	/* op0xfe */
	{ 0xfffc34ff0fffc000ull, 0xf840100000000000ull, USSE_REFUSE, 20 },	/* op0xff */
	{ 0xff70f7c30fe00000ull, 0xf830000000000000ull, USSE_OK, 17 },	/* pcoeff */
	{ 0xfff4f7ff0fffc000ull, 0xfa40000000000000ull, USSE_REFUSE, 20 },	/* phas */
	{ 0xfff7df00fffc0000ull, 0xfa44000000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44010000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44020000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44070000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44080000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44090000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa440a0000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa440f0000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44100000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44110000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44120000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xfff7df00fffc0000ull, 0xfa44170000000000ull, USSE_PHAS, 16 },	/* phas.i */
	{ 0xff7bf7ffffffffffull, 0xf830800000000000ull, USSE_OK, 16 },	/* ptoff */
	{ 0xfffff7ffffffff8full, 0xfd20000000000001ull, USSE_REFUSE, 16 },	/* release */
	{ 0xf87fe7fcf01fffffull, 0xf800010000000000ull, USSE_REFUSE, 21 },	/* savl */
	{ 0xfffbfffffffffefeull, 0xfd10000000000000ull, USSE_OK, 16 },	/* setfc */
	{ 0xf87de7ff3fffc07full, 0xf80000c000000000ull, USSE_REFUSE, 22 },	/* setl */
	{ 0xfffb000000000000ull, 0xfb10000000000000ull, USSE_REFUSE, 16 },	/* smbo */
	{ 0xfff8ffff0fffc000ull, 0xfb18000000000000ull, USSE_REFUSE, 20 },	/* smbo.r */
	{ 0xfffb000000000000ull, 0xfa10000000000000ull, USSE_OK, 16 },	/* smlsi */
	{ 0xfff8ffff0fffc000ull, 0xfa18000000000000ull, USSE_REFUSE, 20 },	/* smlsi.r */
	{ 0xfffb000000000000ull, 0xf810000000000000ull, USSE_REFUSE, 16 },	/* smoa */
	{ 0xfff8ffff0fffc000ull, 0xf818000000000000ull, USSE_REFUSE, 20 },	/* smoa.r */
	{ 0xfffb000000000000ull, 0xf910000000000000ull, USSE_REFUSE, 16 },	/* smr */
	{ 0xfff8ffff0fffc000ull, 0xf918000000000000ull, USSE_REFUSE, 20 },	/* smr.r */
	{ 0xff7cf7ff0fe00000ull, 0xfe28000000000000ull, USSE_REFUSE, 23 },	/* str */
	{ 0xfffff7feffffffffull, 0xf920000000000000ull, USSE_OK, 16 },	/* wdf */
};
