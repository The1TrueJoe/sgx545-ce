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
void pvrsgx_pds_entry(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b, u32 e)
{
	struct pvrsgx_pds p = pvrsgx_pds_prog(sdev, b, 4);
	u32 doutu = pvrsgx_doutu(sdev->fw.code.dev_addr, sdev->fw.addr[PVRSGX_FW_UKERNEL] + e);
	/* The bring-up sweep's variants 5 and 6 (pvrsgx_fw.c), event program only. */
	bool sweep = b == PVRSGX_FW_PDS0;
	u32 w1 = sweep && sdev->sweep_variant == 5 ? 0 : 2;

	/* The temporaries field, in fours (docs/PDS.md, DOUTU word 0). */
	if (sdev->family->ukernel_temps)
		doutu = (doutu & ~PDS_DOUTU_TEMPS) |
			FIELD_PREP(PDS_DOUTU_TEMPS, DIV_ROUND_UP(sdev->family->ukernel_temps, 4));

	if ((sdev->family->features & PVRSGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS) ||
	    (sweep && sdev->sweep_variant == 6)) {
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
	pvrsgx_pds_halt(&p);
}

