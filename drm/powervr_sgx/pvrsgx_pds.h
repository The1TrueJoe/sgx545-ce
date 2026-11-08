/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_PDS_H
#define PVRSGX_PDS_H

#include <linux/types.h>

#include "pvrsgx_device.h"

/*
 * PDS assembly (docs/PDS.md). A program is its data segment in 16-byte rows of
 * DS0 DS0 DS1 DS1, then code. Operands are unified register numbers: DS0 words
 * 0x00-0x3f, DS1 words 0x40-0x7f, inputs from 0x80 (DS0 words 0x40 on).
 */
#define PDS_D0(n)		(n)
#define PDS_D1(n)		(0x40 + (n))
#define PDS_IN(n)		(0x80 + (n))
#define PDS_T0		PDS_D0(0x30)
#define PDS_T1		PDS_D1(0x30)
#define PDS_CC_P0		0
#define PDS_CC_ALWAYS	7
#define PDS_DOUTD		3
#define PDS_DOUTU		5
#define PDS_DOUTA		6
#define PDS_ALU_AND		0xc8
#define PDS_ALU_XOR		0xd0

/* A PDS program being assembled into a firmware buffer. */
struct pvrsgx_pds {
	u32 *w;
	unsigned int data;	/* data segment words */
	unsigned int pc;
};

u32 pvrsgx_doutu(u32 addr);
void pvrsgx_pds_ds(struct pvrsgx_pds *p, unsigned int r, u32 v);
void pvrsgx_pds_movs(struct pvrsgx_pds *p, unsigned int dest, unsigned int a,
		     unsigned int b, unsigned int c, unsigned int d);
void pvrsgx_pds_alu(struct pvrsgx_pds *p, unsigned int cc, unsigned int op,
		    unsigned int d, unsigned int a, unsigned int b);
void pvrsgx_pds_tst(struct pvrsgx_pds *p, unsigned int cc, unsigned int r);
void pvrsgx_pds_bra(struct pvrsgx_pds *p, unsigned int cc, unsigned int target);
void pvrsgx_pds_halt(struct pvrsgx_pds *p);
struct pvrsgx_pds pvrsgx_pds_prog(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b,
				  unsigned int data);
void pvrsgx_pds_entry(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b, u32 e);

#endif /* PVRSGX_PDS_H */
