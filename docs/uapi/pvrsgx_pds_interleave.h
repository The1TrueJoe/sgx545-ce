/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * PDS data-segment interleave: where DS0[k] / DS1[k] land in the data segment.
 *
 * The PDS reads its data store in fixed blocks, and how DS0 and DS1 interleave
 * within a block is a per-core property:
 *
 *   - 2-dword interleave (PVRSGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS, e.g.
 *     SGX545): two words at a time. Each 16-byte row is DS0 DS0 DS1 DS1, so
 *     DS0[k] is at (k>>1)*4 + (k&1) and DS1[k] is 2 words after.
 *   - 8-word interleave (no feature bit, e.g. SGX530): eight words at a time.
 *     Each 64-byte block is DS0[0..7] then DS1[0..7], so DS0[k] is at
 *     (k>>3)*16 + (k&7) and DS1[k] is 8 words after.
 *
 * The PDS instruction encodings and the USSE task words are identical across
 * both; only the byte placement of the data segment differs. A program packed
 * for the wrong interleave has its DOUTU/DOUTD launch sources land on the
 * cores's zero/other words -- on the SGX530 the state-emit DOUTU source (ds0[4])
 * falls on memory word 4 = 0, so the task launches at code base 0 and emits
 * nothing (docs/PDS.md "Program layout"; the §9 vlaunch and the 64x64 board
 * proof, 2026-11-13).
 *
 * Shared by the kernel driver (drm/powervr_sgx) and the frame generator
 * (tools/gen) so the two agree by construction rather than by a second
 * implementation.
 *
 * @reg: 0x00-0x3f selects DS0[reg]; 0x40-0x7f selects DS1[reg - 0x40].
 * @interleave_2dwords: nonzero for the 2-dword layout, zero for the 8-word one.
 *
 * Returns the 32-bit word index of that register within the data segment.
 */
#ifndef PVRSGX_PDS_INTERLEAVE_H
#define PVRSGX_PDS_INTERLEAVE_H

static inline unsigned int pvrsgx_pds_ds_word(unsigned int reg, int interleave_2dwords)
{
	unsigned int k = reg & 0x3f, ds1 = reg >= 0x40 ? 1u : 0u;

	if (interleave_2dwords)
		return (k >> 1) * 4 + ds1 * 2 + (k & 1);
	return (k >> 3) * 16 + ds1 * 8 + (k & 7);
}

/* 16-byte data rows a program of @n0 DS0 words and @n1 DS1 words occupies. */
static inline unsigned int pvrsgx_pds_ds_rows(unsigned int n0, unsigned int n1,
					      int interleave_2dwords)
{
	unsigned int n = n0 > n1 ? n0 : n1;

	return interleave_2dwords ? (n + 1) / 2 : ((n + 7) / 8) * 4;
}

#endif /* PVRSGX_PDS_INTERLEAVE_H */
