/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The state words of vdm.xml, ta.xml, pbe.xml and tex.xml, packed by csbgen's
 * generated helpers (docs/STATE.md sections 2-5).
 */

#ifndef SGX545_CSB_H
#define SGX545_CSB_H

#include <stdint.h>

/* a GPU virtual address */
typedef struct {
   uint32_t addr;
} sgx545_dev_addr_t;

#define SGX545_DEV_ADDR(v) ((sgx545_dev_addr_t){.addr = (v)})

#define __pvr_address_type    sgx545_dev_addr_t
#define __pvr_get_address(a)  ((a).addr)
#define __pvr_make_address(v) SGX545_DEV_ADDR((uint32_t)(v))

#include "csbgen/pbe.h"
#include "csbgen/ta.h"
#include "csbgen/tex.h"
#include "csbgen/vdm.h"

/*
 * Pack one SGX545_<T> at dst, with its fixed bits (the XML's defaults) and
 * the fields the block sets:
 *
 *    sgx545_pack(&w, TA_STATE_ISPB, ispb) {
 *       ispb.line_width = 0x10;
 *    }
 */
#define sgx545_pack(dst, T, name)                                              \
   for (struct SGX545_##T name = {SGX545_##T##_header},                        \
                          *_sgx545_loop_##name = &name;                        \
        _sgx545_loop_##name != NULL;                                           \
        SGX545_##T##_pack((dst), &name), _sgx545_loop_##name = NULL)

#endif
