/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * SGX545 USSE instructions: the encoder and disassembler generated from
 * sgx.xml (docs/ISA.md). The compiler fills a struct sgx_isa_instr and
 * calls sgx_isa_encode().
 *
 * Operands are given by slot, as ISA.md names them: dst, src[0] (src0),
 * src[1] (src1), src[2] (src2). A form with one source (frcp, mov, ...)
 * uses src[1]. Fields a form doesn't have are ignored.
 */

#ifndef SGX_ISA_H
#define SGX_ISA_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sgx-isa-enums.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sgx_isa_file {
   SGX_ISA_FILE_NONE = 0,
   SGX_ISA_FILE_R,   /* temporaries */
   SGX_ISA_FILE_O,   /* outputs */
   SGX_ISA_FILE_PA,  /* primary attributes */
   SGX_ISA_FILE_SA,  /* secondary attributes */
   SGX_ISA_FILE_IX,  /* ixbank[idx<ixreg> + num] */
   SGX_ISA_FILE_C,   /* hardware constants c0-c63 */
   SGX_ISA_FILE_G,   /* special registers */
   SGX_ISA_FILE_IMM, /* #num */
   SGX_ISA_FILE_I,   /* internal registers i0-i3 */
   SGX_ISA_FILE_IDX, /* index registers (destination only) */
};

enum sgx_isa_fmt {
   SGX_ISA_FMT_NONE = 0,
   SGX_ISA_FMT_F16,
   SGX_ISA_FMT_C10,
};

struct sgx_isa_src {
   uint8_t file; /* enum sgx_isa_file */
   uint8_t fmt; /* enum sgx_isa_fmt: number bit 6 under format select/control */
   uint8_t half;   /* F16 half (0 lo, 1 hi) where F16 numbers count halves */
   uint8_t ixbank; /* IX: 0 r, 1 o, 2 pa, 3 sa */
   uint8_t ixreg;  /* IX: 1 or 2 */
   uint8_t lane;   /* pck .l<n> */
   bool neg, abs;
   uint32_t num; /* register, constant, immediate, or IX offset */
};

struct sgx_isa_instr {
   enum sgx_isa_opc opc;

   /* Common */
   uint8_t
      pred; /* the class's predicate field value (enum sgx_isa_pred3/pred2) */
   bool skipinv, nosched, syncs, end;
   bool fmt; /* format select */
   bool moe, inc[3];
   bool rcnt;    /* repeat-count form (classes with h21 rcnt) */
   uint8_t rpt;  /* repeat count; 0 and 1 both mean once */
   uint8_t mask; /* write mask when there is no repeat count */
   uint8_t drc;

   struct sgx_isa_src dst, src[3];

   /* float */
   bool f16d;
   bool sf16,
      sc10;      /* hw 1: the source is F16 (h7) / C10 (h8); both may be set */
   uint8_t chan; /* hw1 chan, test chan */
   uint8_t swz[3]; /* fmad16, enum sgx_isa_f16swz */
   uint8_t cp[2];  /* fdpc / fddpc clip planes */
   uint8_t idst;   /* fddp / fddpc second destination i0/i1 */

   /* moves */
   uint8_t cond; /* movc, enum sgx_isa_movc_cond */
   bool f1_1, f1_26;
   uint8_t a3f;

   /* pck */
   uint8_t dfmt, sfmt; /* enum sgx_isa_pck_fmt */
   uint8_t dmask;
   bool scale; /* l18: scale, or rint for float pairs */
   uint8_t rnd;

   /* test / testmask */
   uint8_t alu;   /* enum sgx_isa_test_alu */
   uint8_t tcond; /* enum sgx_isa_test_cond */
   uint8_t pdst;
   uint8_t ttype;    /* enum sgx_isa_testmask_type */
   bool has_nodst;   /* without a destination, the dst field holds nodst */
   uint16_t nodst;   /* (default 0x280 when has_nodst is false) */
   bool has_unused2; /* rlp: src2's slot as one 10-bit value is unused2 */
   uint16_t unused2; /* (default 0x300, the unused pattern, when has_unused2 is
                        false) */
   bool f2_0;

   /* bitwise */
   bool inv, partial;
   uint8_t rot;

   /* smp */
   uint8_t dim, lod, cfmt, pcf, ret;

   /* ld / st / eld */
   uint8_t am, size;
   bool range, preinc, postinc, f2_2, f2_3, f2_9, f3_13;

   /* special */
   int32_t target; /* branch target, phas.i pc */
   bool link, f1_22, syncend, xsyncend, f2_26, f3_5;
   int16_t v[4];   /* smoa/smr/smbo/imo/smlsi values */
   uint8_t am4[4]; /* smoa modes */
   bool swzf[4];   /* smlsi: value is a swizzle */
   uint8_t lim[3]; /* smlsi */
   uint8_t a[2];   /* setfc */
   uint8_t n;
   uint8_t path, tgt;
   bool f10, f12, f13, f3_20, f3_6, f3_11, f3_12;
   uint32_t imm; /* limm, emit, eld immediates */
   bool sel34, sel36, sel37, sel43, pw, f2_25;
   uint8_t temps4, wait, rate;

   /*
    * Color / integer classes (sop2, sopwm, sop3 family, dot, ima8, fpma):
    * each select is a value and a complement flag ("1-x").
    */
   uint8_t c0, c1, c2, a0, a1, a2, asel;
   bool c0c, c1c, c2c, a0c, a1c, a2c, aselc;
   uint8_t cop, aop;
   bool cs1c, as1c, aneg, arev, cneg;
   uint8_t l1, l2, lcs, p; /* lrp1, sop3x */
   bool l1c, l2c;
   bool f4, f6, s1c, s2c, s1ac; /* dot */
   uint8_t dcs, das;            /* dot color / alpha scale */

   /* ima16 / imae / ima32 */
   bool sat, sgn, dflag1, s0hi, s1hi, s2hi, cout, cin, ci, negp;
   uint8_t f2_15, f1, f2, t2, hi, cin3;

   /* hw 16 / 17 */
   uint8_t ifmt, x39, ii, a5, coef, shift, a8;
   bool ileave, src23, dst23, irnd, x1b, x1c, x1d, nf16, s8, x38, s0f13, f2_27;

   /* efo */
   uint8_t efo_d, efo_i, efo_a, efo_m;
   bool wi0, wi1, a0lneg, a0rneg, f3_10;

   /* fdual */
   uint8_t pri, sec, sd, sb, sc, msrc;
};

/* Cores sgx.xml describes; the value is the core's number. */
enum sgx_isa_core {
   SGX_ISA_CORE_SGX545 = 545,
};

/*
 * What an encoding depends on besides the instruction: the target core and
 * format control (setfc), program state the color/integer classes read.
 */
struct sgx_isa_fc {
   bool c10;      /* setfc a1: on at program start */
   bool f16;      /* setfc a0 (efo): off at program start */
   uint16_t core; /* enum sgx_isa_core; 0 is SGX545 */
};

#define SGX_ISA_FC_START ((struct sgx_isa_fc){.c10 = true, .f16 = false})

/* Whether sgx.xml describes the core (0 counts as SGX545). */
bool sgx_isa_core_supported(unsigned core);

/*
 * What sgx.xml's expressions read as ISA_GPU_ID() and its <gen> ranges
 * compare: core * 100 + format control (bit 0 C10, bit 1 F16).
 */
uint32_t sgx_isa_gpu_id(const struct sgx_isa_fc *fc);

/* If word is a setfc, apply it to *fc and return true. */
bool sgx_isa_setfc(uint64_t word, struct sgx_isa_fc *fc);

/* Encode one instruction. fc may be NULL (program-start state). */
uint64_t sgx_isa_encode(const struct sgx_isa_instr *instr,
                        const struct sgx_isa_fc *fc);

/*
 * Disassemble n instructions (low word first) to out, one per line.
 * Format control is tracked through setfc from fc (NULL: program start).
 */
void sgx_disasm(const uint32_t *words, unsigned n, FILE *out,
                const struct sgx_isa_fc *fc);

/* One instruction to a string (no newline); false if nothing matched. */
bool sgx_disasm_one(uint64_t word, const struct sgx_isa_fc *fc, char *buf,
                    size_t size);

/*
 * Co-issue (ISA.md "fdual for a compiler", "efo"). What an fdual's two
 * halves read and write: bit n of an _i mask is i<n> (r124+n counts as
 * i<n>), bit k of a _slots mask the src<k> slot. settled is false where
 * ISA.md leaves the op's operands open (fmaxmin, fmad16 and ima32 as the
 * secondary, fddp and ima32 as the primary); the masks then assume the most.
 */
struct sgx_isa_fdual_use {
   uint8_t sec_reads_i, sec_writes_i, sec_reads_slots;
   uint8_t pri_reads_i, pri_writes_i, pri_reads_slots;
   bool settled;
};

bool sgx_isa_fdual_use(const struct sgx_isa_instr *instr,
                       struct sgx_isa_fdual_use *use);

/*
 * A same-word race the hardware shows (an fdual primary reading an internal
 * register its secondary writes; efo's d naming I0 while the word writes I0):
 * a reason, or NULL. Not hazards, by ISA.md: both halves reading one slot, the
 * secondary reading an internal register the primary writes, the next
 * instruction reading the secondary's result, efo's d=i1 with wi1 (it reads
 * the old I1).
 */
const char *sgx_isa_coissue_hazard(const struct sgx_isa_instr *instr);

#ifdef __cplusplus
}
#endif

#endif
