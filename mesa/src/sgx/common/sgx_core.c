/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "sgx_core.h"

static const struct sgx_core sgx_cores[] = {
   {
      /* Intel CE5300 (revision 1.0.14) and Cedarview (1.0.13) */
      .name = "SGX545",
      .id = 0x0115,
      .rev_min = 0x0001000d,
      .rev_max = 0x0001000e,
      .usse = SGX_USSE1,
      .verified_on = "Intel CE5300, Intel Cedarview (N2600)",

      .max_texture_size = 2048,
      .max_cube_levels = 12,
      .samplers = 8,
      .vs_inputs = 16,
      .vs_outputs = 11,
      .fs_inputs = 10,
      .gl_max_temps = 64,
      .temps = 124,
      .coord_sets = 10,
      .rt_max_width = 2048,
      .rt_max_height = 2048,
      .rt_max_samples = 1,
      .line_width_max = 16,
      .point_size_max = 511,

      .vertex_task_dwords = 128,
      .vertex_task_max = 16,
      .dma_chunk_dwords = 16,

      .f16 = true,
      .f16_sample_return = true,

      .sample_texture_heap_only = true,
      .npot_mip_hangs = true,
   },
};

const struct sgx_core *
sgx_core_lookup(uint32_t core_id, uint32_t core_revision)
{
   for (size_t i = 0; i < sizeof(sgx_cores) / sizeof(sgx_cores[0]); i++) {
      const struct sgx_core *c = &sgx_cores[i];

      if (c->id == core_id >> 16 && core_revision >= c->rev_min &&
          core_revision <= c->rev_max)
         return c;
   }
   return NULL;
}
