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
#define PDS_CC_ALWAYS	7
#define PDS_DOUTU		5

/* A PDS program being assembled into a firmware buffer. */
struct pvrsgx_pds {
	u32 *w;
	unsigned int data;	/* data segment words */
	unsigned int pc;
};

u32 pvrsgx_doutu(u32 kcode, u32 addr);
void pvrsgx_pds_ds(struct pvrsgx_pds *p, unsigned int r, u32 v);
void pvrsgx_pds_movs(struct pvrsgx_pds *p, unsigned int dest, unsigned int a,
		     unsigned int b, unsigned int c, unsigned int d);
void pvrsgx_pds_halt(struct pvrsgx_pds *p);
struct pvrsgx_pds pvrsgx_pds_prog(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b,
				  unsigned int data);
void pvrsgx_pds_entry(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b, u32 e);
u32 pvrsgx_pds_f1_addr(struct pvrsgx_device *sdev);

#endif /* PVRSGX_PDS_H */
