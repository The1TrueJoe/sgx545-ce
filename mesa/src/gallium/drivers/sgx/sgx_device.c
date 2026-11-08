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
};

const struct sgx_device_features *
sgx_device_features(const struct sgxws_device_info *info)
{
   for (unsigned i = 0; i < ARRAY_SIZE(sgx_devices); i++) {
      if (sgx_devices[i].core_id == info->core_id >> 16)
         return &sgx_devices[i];
   }
   return NULL;
}
