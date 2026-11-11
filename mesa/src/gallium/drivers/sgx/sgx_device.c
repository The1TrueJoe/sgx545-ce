/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include "util/macros.h"

#include "sgx_device.h"
#include "sgxws.h"

static const struct sgx_device_features sgx_devices[] = {
   {
      /* the CE5300's: core id 0x01150000, revision 1.0.14 (docs/UAPI.md
       * "DEV_QUERY") */
      .name = "SGX545",
      .core_id = 0x0115,
      .max_texture_size = 2048,
      .max_cube_levels = 12,
      .samplers = 8,
      .vs_inputs = 16,
      .vs_outputs = 11,
      .fs_inputs = 10,
      .max_temps = 64,
      .f16 = true,
   },
   {
      /* Our OMAP3630 test board's: revision 1.2.5 expected (0x00010205).
       * docs/porting/cores/sgx530.md has each fact and its tag; nothing is
       * confirmed on the hardware yet (no C tag), so every value below but
       * core_id is the SGX545's or the safe choice:
       *   core_id      unknown (gap 1). 0 matches nothing, so the entry is
       *                inert until the first ID read fills it in.
       *   limits       the SGX545's, unconfirmed (gap 12).
       *   f16          off: F16 and C10 are unknown on this core (gap 10),
       *                so the compiler emits F32 only.
       * The USSE encodings are the SGX545's (one sgx.xml, no SGX530 gen yet;
       * docs/porting/isa.md), checked by hwt_open on the board (gap 10). */
      .name = "SGX530",
      .core_id = 0,
      .max_texture_size = 2048,
      .max_cube_levels = 12,
      .samplers = 8,
      .vs_inputs = 16,
      .vs_outputs = 11,
      .fs_inputs = 10,
      .max_temps = 64,
      .f16 = false,
      .provisional = true,
   },
};

const struct sgx_device_features *
sgx_device_features(const struct sgxws_device_info *info)
{
   for (unsigned i = 0; i < ARRAY_SIZE(sgx_devices); i++) {
      if (sgx_devices[i].core_id &&
          sgx_devices[i].core_id == info->core_id >> 16)
         return &sgx_devices[i];
   }
   return NULL;
}
