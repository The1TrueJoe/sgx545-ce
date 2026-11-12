// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The PDS programs that enter the microkernel, assembled at load
 * (docs/PDS.md for the encoding, docs/SRVINIT.md for the programs).
 */

#include <linux/bitfield.h>
#include <linux/math.h>

#include "pvrsgx_fw.h"
#include "pvrsgx_pds.h"

/* DOUTU word 0: temporary registers, in fours. */
#define PDS_DOUTU_TEMPS		GENMASK(31, 24)
/* MOVS: the DOUT destination and the condition code (docs/PDS.md). */
#define PDS_MOVS_DEST		GENMASK(4, 0)
#define PDS_MOVS_CC		GENMASK(26, 24)

/*
 * DOUTU word 0 for microkernel code at device address a, in the kernel code
 * heap at kcode: code base 15 plus the 512 KiB segment, then the offset in the
 * segment in 8-byte instructions, << 4. Bit 27 is set on every microkernel
 * entry.
 */
u32 pvrsgx_doutu(u32 kcode, u32 a)
{
	u32 off = a - kcode, seg = off >> 19;

	return ((seg + 15) & 15) | 0x8000000 | (((off - (seg << 19)) * 2) & 0x3ffff0);
}

void pvrsgx_pds_ds(struct pvrsgx_pds *p, unsigned int r, u32 v)
{
	unsigned int k = r & 0x3f;

	p->w[(k >> 1) * 4 + (r >= 0x40 ? 2 : 0) + (k & 1)] = v;
}

static void pds_emit(struct pvrsgx_pds *p, u32 i)
{
	p->w[p->data + p->pc++] = i;
}

static unsigned int ds0w(unsigned int r)
{
	return r >= 0x80 ? r - 0x40 : r;
}

/* MOVS: four words from one DS0 pair and one DS1 pair to a DOUT destination. */
void pvrsgx_pds_movs(struct pvrsgx_pds *p, unsigned int dest, unsigned int a,
		     unsigned int b, unsigned int c, unsigned int d)
{
	unsigned int src[4] = { a, b, c, d }, r1 = 0, r2 = 0, sel = 0;

	for (int i = 0; i < 4; i++) {
		unsigned int s;

		if (src[i] >= 0x40 && src[i] < 0x80) {
			r2 = (src[i] - 0x40) >> 1;
			s = 2 + (src[i] & 1);
		} else {
			r1 = ds0w(src[i]) >> 1;
			s = ds0w(src[i]) & 1;
		}
		sel |= s << (11 - 2 * i);
	}
	pds_emit(p, (u32)PDS_CC_ALWAYS << 24 | r1 << 18 | r2 << 13 | sel | dest);
}

void pvrsgx_pds_halt(struct pvrsgx_pds *p)
{
	pds_emit(p, 0xaf000000u);
}

struct pvrsgx_pds pvrsgx_pds_prog(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b,
				  unsigned int data)
{
	return (struct pvrsgx_pds){ sdev->fw.cpu[b], data, 0 };
}

/* Start the microkernel at offset e: DOUTU with word 1 = 2. */
/*
 * The stage-1 sweep's minimal event programs (docs/PDS.md encodings): does the
 * SGX530's PDS execute at all? Its PC reads back by 16-byte rows, so each test
 * puts its landmark in a row of its own. A: a branch from row 1 into row 3,
 * which branches to itself, so a running PDS sits on row 3. C: a branch from
 * row 1 to row 2, which halts. D: A from byte 0, with no data segment. They
 * run over PDS1's slot, which no SGX530 event register uses.
 *
 * F1 and F2 detect execution by a BIF fault at an address only that
 * instruction touches. F1: a one-dword DOUTD from PVRSGX_PDS_F1_ADDR past the
 * PDS vertex heap, where nothing is mapped. F2: a branch 0x10000 bytes past
 * the first code word, beyond the code arena and the PD table. HALT: the
 * data row and a HALT, no DOUT.
 */
#define BRA(t)	(0x97000000 | (t))
#define HALT	0xaf000000
#define PDS_DOUTD	3
#define PDS_DOUTA	6

static const u32 sweep_prog[][12] = {
	[1] = { BRA(8), HALT, HALT, HALT, HALT, HALT, HALT, HALT, BRA(8), HALT, HALT, HALT },
	[2] = { BRA(4), HALT, HALT, HALT, HALT, HALT, HALT, HALT },
	[3] = { BRA(8), HALT, HALT, HALT, HALT, HALT, HALT, HALT, BRA(8), HALT, HALT, HALT },
	[PVRSGX_SWEEP_PROG_F2] = { BRA(0x4000), HALT, HALT, HALT },
	[PVRSGX_SWEEP_PROG_HALT] = { HALT },
};

#undef BRA
#undef HALT

/* F1's DOUTD source: in the hole past the PDS vertex heap, not the fault check's. */
u32 pvrsgx_pds_f1_addr(struct pvrsgx_device *sdev)
{
	return (u32)pvrsgx_heaps(sdev)->pds_vertex.end + 0x40000;
}

static void pds_sweep_prog(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b)
{
	struct pvrsgx_pds p = pvrsgx_pds_prog(sdev, b, sdev->sweep_prog == 3 ? 0 : 4);

	if (sdev->sweep_prog == PVRSGX_SWEEP_PROG_F1) {
		/* [addr, 0, 0, 0]: (addr, ctrl 0 = one dword) under either interleave. */
		pvrsgx_pds_ds(&p, PDS_D0(0), pvrsgx_pds_f1_addr(sdev));
		pvrsgx_pds_ds(&p, PDS_D0(1), 0);
		pvrsgx_pds_ds(&p, PDS_D1(0), 0);
		pvrsgx_pds_ds(&p, PDS_D1(1), 0);
		pvrsgx_pds_movs(&p, PDS_DOUTD, PDS_D0(0), PDS_D1(1), PDS_D0(1), PDS_D0(0));
		pvrsgx_pds_halt(&p);
		return;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(sweep_prog[0]); i++)
		if (sweep_prog[sdev->sweep_prog][i])
			pds_emit(&p, sweep_prog[sdev->sweep_prog][i]);
}

void pvrsgx_pds_entry(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b, u32 e)
{
	struct pvrsgx_pds p = pvrsgx_pds_prog(sdev, b, 4);
	u32 doutu = pvrsgx_doutu(sdev->fw.code.dev_addr, sdev->fw.addr[PVRSGX_FW_UKERNEL] + e);
	/* The bring-up sweep's changes (pvrsgx_fw.c), event program only. */
	u32 sw = b == PVRSGX_FW_PDS0 ? sdev->sweep_flags : 0;
	u32 w1 = sw & PVRSGX_SWEEP_DOUTU_W1_0 ? 0 : 2;

	if (b == PVRSGX_FW_PDS0 && sdev->sweep_prog && sdev->sweep_prog < ARRAY_SIZE(sweep_prog)) {
		pds_sweep_prog(sdev, b);
		return;
	}

	/* The temporaries field, in fours (docs/PDS.md, DOUTU word 0). */
	if (sdev->family->ukernel_temps)
		doutu = (doutu & ~PDS_DOUTU_TEMPS) |
			FIELD_PREP(PDS_DOUTU_TEMPS, DIV_ROUND_UP(sdev->family->ukernel_temps, 4));
	if (sw & (PVRSGX_SWEEP_TEMPS4 | PVRSGX_SWEEP_TEMPS0))
		doutu = (doutu & ~PDS_DOUTU_TEMPS) |
			FIELD_PREP(PDS_DOUTU_TEMPS, sw & PVRSGX_SWEEP_TEMPS4 ? 1 : 0);
	if (sw & PVRSGX_SWEEP_TEMPS_FIELD)
		doutu = (doutu & ~PDS_DOUTU_TEMPS) |
			FIELD_PREP(PDS_DOUTU_TEMPS, sdev->sweep_doutu0);
	/* The sweep's bit-weight test: the temporaries and one address bit only. */
	if (b == PVRSGX_FW_PDS0 && sdev->sweep_doutu_bit)
		doutu = (doutu & PDS_DOUTU_TEMPS) | BIT(sdev->sweep_doutu_bit - 1);
	if (b == PVRSGX_FW_PDS0 && sdev->sweep_doutu_set)
		doutu = (doutu & PDS_DOUTU_TEMPS) | sdev->sweep_doutu0;
	/* The sweep's extra bits on the normal word, e.g. a temporaries field. */
	if (sw & PVRSGX_SWEEP_DOUTU_OR)
		doutu |= sdev->sweep_doutu0;

	if (sw & PVRSGX_SWEEP_PDS3_MIRROR) {
		/*
		 * The SGX545's pds3 layout (docs/SRVINIT.md): DOUTU 0x07003885, so
		 * (ds1[3] = doutu, ds0[0] = 2, ds0[1]), thirteen tst ds0[0] after the
		 * DOUTA, as pds3 spaces them. The DOUTA's value is in ds0[2]. Words
		 * 0, 4 and 7 hold 2, the value and doutu under either interleave;
		 * 1 and 2, which swap between them, are both 0. Or pds3's own
		 * DOUTA form, 0x07800406: the value from in0.
		 */
		p = pvrsgx_pds_prog(sdev, b, 8);
		pvrsgx_pds_ds(&p, PDS_D0(0), 2);
		pvrsgx_pds_ds(&p, PDS_D0(1), 0);
		pvrsgx_pds_ds(&p, PDS_D1(0), 0);
		pvrsgx_pds_ds(&p, PDS_D1(1), 0);
		pvrsgx_pds_ds(&p, PDS_D0(2), sdev->sweep_doutu0);
		pvrsgx_pds_ds(&p, PDS_D0(3), 0);
		pvrsgx_pds_ds(&p, PDS_D1(2), 0);
		pvrsgx_pds_ds(&p, PDS_D1(3), doutu);
		if (sw & PVRSGX_SWEEP_PDS3_IN0)
			pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_IN(0), PDS_D1(0), PDS_IN(0), PDS_IN(0));
		else
			pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_D0(2), PDS_D1(0), PDS_D0(2),
					PDS_D0(2));
		for (unsigned int i = 0; i < 13; i++)
			pds_emit(&p, 0x87000000);	/* tst ds0[0] */
		pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D1(3), PDS_D0(0), PDS_D0(1), PDS_D0(0));
		pvrsgx_pds_halt(&p);
		return;
	}
	if (sw & PVRSGX_SWEEP_DOUTA_1ROW) {
		/*
		 * One row, [value, 0, 0, doutu]: words 0 and 3 sit in the same
		 * place under either interleave (the 1- and 2-dword layouts swap
		 * only 1 and 2), so DOUTA gets (value, 0) and DOUTU (doutu, 0, 0).
		 */
		/* Or the address of the control block's version word, t[0x18]. */
		pvrsgx_pds_ds(&p, PDS_D0(0), sw & PVRSGX_SWEEP_DOUTA_CTLBLK ?
			      sdev->fw.addr[PVRSGX_FW_TA3DCTL] + 0x18 * 4 : sdev->sweep_doutu0);
		pvrsgx_pds_ds(&p, PDS_D0(1), 0);
		pvrsgx_pds_ds(&p, PDS_D1(0), 0);
		pvrsgx_pds_ds(&p, PDS_D1(1), doutu);
		pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_D0(0), PDS_D0(1), PDS_D0(0), PDS_D0(1));
		/* The .x2.s27 form seen on DOUTA pairs (0x2f..., docs/PDS.md). */
		if (sw & PVRSGX_SWEEP_DOUTA_X2)
			p.w[p.data + p.pc - 1] |= 0x28000000;
		pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D1(1), PDS_D0(1), PDS_D1(0), PDS_D1(1));
		pvrsgx_pds_halt(&p);
		return;
	}
	if (sw & PVRSGX_SWEEP_DOUTA_PA0) {
		/*
		 * Two data rows: (value, ctrl 0 = PA0) for a DOUTA (docs/PDS.md),
		 * then the DOUTU's (doutu, 0, 0, w1). Event data 0 = 2 starts the
		 * code after them; it runs over PDS1's slot, unused on the SGX530.
		 */
		p = pvrsgx_pds_prog(sdev, b, 8);
		pvrsgx_pds_ds(&p, PDS_D0(0), sdev->sweep_doutu0);
		pvrsgx_pds_ds(&p, PDS_D0(1), 0);
		pvrsgx_pds_ds(&p, PDS_D1(0), 0);
		pvrsgx_pds_ds(&p, PDS_D1(1), 0);
		pvrsgx_pds_ds(&p, PDS_D0(2), doutu);
		pvrsgx_pds_ds(&p, PDS_D0(3), 0);
		pvrsgx_pds_ds(&p, PDS_D1(2), 0);
		pvrsgx_pds_ds(&p, PDS_D1(3), w1);
		pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_D0(0), PDS_D0(1), PDS_D0(0), PDS_D0(1));
		pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D0(2), PDS_D1(3), PDS_D0(3), PDS_D0(2));
		pvrsgx_pds_halt(&p);
		return;
	}
	if (sw & PVRSGX_SWEEP_ALL_SLOTS) {
		/* The DOUTU gets (doutu, doutu, doutu) under any data layout. */
		pvrsgx_pds_ds(&p, PDS_D0(0), doutu);
		pvrsgx_pds_ds(&p, PDS_D0(1), doutu);
		pvrsgx_pds_ds(&p, PDS_D1(0), doutu);
		pvrsgx_pds_ds(&p, PDS_D1(1), doutu);
		pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D0(0), PDS_D0(0), PDS_D0(0), PDS_D0(0));
	} else if ((sdev->family->features & PVRSGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS) ||
		   (sw & PVRSGX_SWEEP_INTERLEAVE)) {
		pvrsgx_pds_ds(&p, PDS_D0(0), doutu);
		pvrsgx_pds_ds(&p, PDS_D0(1), w1);
		pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D0(0), PDS_D0(1), PDS_D1(0), PDS_D0(0));
	} else {
		/*
		 * Whether the data store interleaves one word at a time is unknown
		 * (G): the memory row [doutu, 0, 0, 2] gives DOUTU (doutu, 2, 0)
		 * under either layout.
		 */
		pvrsgx_pds_ds(&p, PDS_D0(0), doutu);
		pvrsgx_pds_ds(&p, PDS_D0(1), 0);
		pvrsgx_pds_ds(&p, PDS_D1(0), 0);
		pvrsgx_pds_ds(&p, PDS_D1(1), w1);
		pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D0(0), PDS_D1(1), PDS_D0(1), PDS_D0(0));
	}
	/* The sweep's MOVS destination (b0-4) and condition (b24-26), event program only. */
	if (b == PVRSGX_FW_PDS0 && sdev->sweep_dest)
		p.w[p.data + p.pc - 1] = (p.w[p.data + p.pc - 1] & ~PDS_MOVS_DEST) |
					  FIELD_PREP(PDS_MOVS_DEST, sdev->sweep_dest - 1);
	if (b == PVRSGX_FW_PDS0 && sdev->sweep_cc)
		p.w[p.data + p.pc - 1] = (p.w[p.data + p.pc - 1] & ~PDS_MOVS_CC) |
					  FIELD_PREP(PDS_MOVS_CC, sdev->sweep_cc - 1);
	pvrsgx_pds_halt(&p);
}

