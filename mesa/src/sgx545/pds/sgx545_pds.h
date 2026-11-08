/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * SGX545 PDS programs (docs/PDS.md): the encoder and disassembler generated
 * from sgx545_pds.xml, the DOUT data-word encoders, and the program layout
 * (data segment, then code).
 */

#ifndef SGX545_PDS_H
#define SGX545_PDS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sgx545pds-isa-enums.h"

#ifdef __cplusplus
extern "C" {
#endif

/* condition field values */
#define SGX545_PDS_CC_P0     0   /* only if p0 */
#define SGX545_PDS_CC_3      3   /* the vertex-fetch DOUTU form */
#define SGX545_PDS_CC_ALWAYS 7

/* DOUT targets */
enum sgx545_pds_dout {
   SGX545_PDS_DOUTI = 2,   /* iterator setup, 2 words */
   SGX545_PDS_DOUTD = 3,   /* DMA into USSE attributes, 2 words (3 with x2.s27) */
   SGX545_PDS_DOUTT = 4,   /* texture state, 4 words */
   SGX545_PDS_DOUTU = 5,   /* start the USSE task, 3 words */
   SGX545_PDS_DOUTA = 6,   /* write a primary attribute register, 2 words */
};

/* MOVS selects: word n of a DOUT comes from */
enum sgx545_pds_sel {
   SGX545_PDS_S1_LO = 0,   /* the DS0 pair's first word */
   SGX545_PDS_S1_HI = 1,
   SGX545_PDS_S2_LO = 2,   /* the DS1 pair's first word */
   SGX545_PDS_S2_HI = 3,
};

enum sgx545_pds_file {
   SGX545_PDS_FILE_DS0,
   SGX545_PDS_FILE_DS1,
   SGX545_PDS_FILE_IN,     /* inputs: in0 vertex index, in1 instance index, ... */
};

/* A data-store word (U operands), or one of its 16-bit halves (S1, S2). */
struct sgx545_pds_reg {
   uint8_t file;           /* enum sgx545_pds_file */
   uint8_t num;            /* word */
   bool hi;                /* S1/S2: the high half */
};

#define SGX545_PDS_DS0(n) ((struct sgx545_pds_reg){ SGX545_PDS_FILE_DS0, (n) })
#define SGX545_PDS_DS1(n) ((struct sgx545_pds_reg){ SGX545_PDS_FILE_DS1, (n) })
#define SGX545_PDS_IN(n)  ((struct sgx545_pds_reg){ SGX545_PDS_FILE_IN, (n) })

struct sgx545_pds_instr {
   enum sgx545_pds_opc opc;
   uint8_t cc;             /* SGX545_PDS_CC_* */

   /* movs */
   uint8_t dest;           /* enum sgx545_pds_dout */
   uint8_t r1, r2;         /* DS0 pair (words 2r, 2r+1; 0x20+ inputs), DS1 pair */
   uint8_t sel[4];         /* enum sgx545_pds_sel, per word sent */
   bool x2, s27;           /* both: DOUTD adds word 2 to its address */

   /* add, mul16, and, or, xor, op0xd.s1 (dst U, src1 S1, src2 S2),
    * tst (src1 or, tst.s2, src2), shl/shr (dst U, src1 U) */
   struct sgx545_pds_reg dst, src1, src2;
   uint8_t amount;         /* shift count */
   uint32_t target;        /* bra: instruction index */
};

uint32_t sgx545_pds_encode(const struct sgx545_pds_instr *instr);

/* One instruction to text; false if nothing matched. */
bool sgx545_pds_disasm_one(uint32_t word, char *buf, size_t size);
/* n instructions, one per line. */
void sgx545_pds_disasm(const uint32_t *code, unsigned n, FILE *out);
/*
 * A whole program as it sits in memory (data rows of DS0 DS0 DS1 DS1, then
 * code): the data words by store, then the code to the last HALT.
 */
void sgx545_pds_disasm_program(const uint32_t *words, unsigned rows, unsigned n_code, FILE *out);

/* Text (the disassembler's notation) to an instruction; NULL or an error. */
const char *sgx545_pds_asm(const char *text, struct sgx545_pds_instr *instr);

/* ---- DOUT data words ---- */

struct sgx545_pds_doutu {
   /* word 0 */
   uint8_t base;           /* USE code base register */
   uint32_t offset;        /* code offset in its 512 KB window, bytes (multiple of 8) */
   uint8_t temps4;         /* bits 24-31: temporaries in fours */
   /* word 1 */
   bool b1;                /* microkernel tasks */
   bool pixel;             /* pixel tasks (b2) */
   bool b22, b23;
   bool iterations;        /* the pixel program has plain iterations */
   bool samples;           /* ... texture samples */
};

uint32_t sgx545_pds_doutu_w0(const struct sgx545_pds_doutu *u);
uint32_t sgx545_pds_doutu_w1(const struct sgx545_pds_doutu *u);

/* DOUTD word 1 (word 0 is the address, word 2 with x2.s27 an offset) */
struct sgx545_pds_doutd {
   uint32_t dwords;        /* 1-256 */
   uint32_t reg;           /* destination attribute register */
   uint8_t kind;           /* bits 22-24 */
   bool secondary;         /* bit 25 */
};

uint32_t sgx545_pds_doutd_ctrl(const struct sgx545_pds_doutd *d);

struct sgx545_pds_douti {
   /* sample half */
   uint8_t sample_set;     /* texture coordinate set + 1, 0 = no sample */
   bool dim2, proj;
   bool last_sample;
   /* iterate half */
   uint8_t src;            /* set + 1; 0 position, 0x20 colour, 0x22 depth plane, 0x24 pure sample */
   bool colour_prec;       /* b21 */
   uint8_t comps;          /* 1-4 */
   bool iterate;           /* plain iteration enabled */
   uint8_t b25_26;         /* 3 from GL's builder, 0 on the clear's colour and the background's sample */
   bool prec2, prec1;      /* b27, b28 (mediump: prec1) */
   bool last_iter;
   /* word 1 */
   uint8_t w1;             /* bits 0-6; 0x10 = a sprite's gl_PointCoord */
};

uint32_t sgx545_pds_douti_w0(const struct sgx545_pds_douti *i);
uint32_t sgx545_pds_douti_w1(const struct sgx545_pds_douti *i);

/* DOUTA word 1 (word 0 is the value) */
struct sgx545_pds_douta {
   uint8_t reg;            /* primary attribute register */
};

uint32_t sgx545_pds_douta_ctrl(const struct sgx545_pds_douta *a);

/* ---- program layout ---- */

/* DS0 / DS1 word n in a program's memory words (rows of DS0 DS0 DS1 DS1) */
static inline unsigned
sgx545_pds_ds0_at(unsigned n)
{
   return (n >> 1) * 4 + (n & 1);
}

static inline unsigned
sgx545_pds_ds1_at(unsigned n)
{
   return (n >> 1) * 4 + 2 + (n & 1);
}

#ifdef __cplusplus
}
#endif

#endif
