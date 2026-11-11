/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The state words of vdm.xml, ta.xml, pbe.xml and tex.xml, packed by csbgen's
 * generated helpers.
 */

#ifndef SGX_CSB_H
#define SGX_CSB_H

#include <stdint.h>

/* a GPU virtual address */
typedef struct {
   uint32_t addr;
} sgx_dev_addr_t;

#define SGX_DEV_ADDR(v) ((sgx_dev_addr_t){.addr = (v)})

#define __pvr_address_type    sgx_dev_addr_t
#define __pvr_get_address(a)  ((a).addr)
#define __pvr_make_address(v) SGX_DEV_ADDR((uint32_t)(v))

#include "csbgen/pbe.h"
#include "csbgen/ta.h"
#include "csbgen/tex.h"
#include "csbgen/vdm.h"

/*
 * Pack one SGX_<T> at dst, with its fixed bits (the XML's defaults) and
 * the fields the block sets:
 *
 *    sgx_pack(&w, TA_STATE_ISPB, ispb) {
 *       ispb.line_width = 0x10;
 *    }
 */
#define sgx_pack(dst, T, name)                                                 \
   for (struct SGX_##T name = {SGX_##T##_header}, *_sgx_loop_##name = &name;   \
        _sgx_loop_##name != NULL;                                              \
        SGX_##T##_pack((dst), &name), _sgx_loop_##name = NULL)

#endif
