/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_DEVICE_H
#define SGX_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

struct sgxws_device_info;

/*
 * What a core can do that the kernel doesn't report.  The kernel gives the
 * core's id and revision and its render-target limits (sgxws_dev_info); the
 * id picks one of these.  The SGX5xx cores differ in their USSE instruction
 * set and state words (SGX530/531/535/540 against SGX543/544/554), so a core
 * without an entry isn't driven at all.
 */
struct sgx_device_features {
   const char *name;          /* "SGX545" */
   uint16_t core_id;          /* EUR_CR_CORE_ID bits 16-31 */
   unsigned max_texture_size; /* texels a side, 2-D and cube */
   unsigned max_cube_levels;
   unsigned samplers;              /* texture units */
   unsigned vs_inputs, vs_outputs; /* vec4 slots */
   unsigned fs_inputs;
   unsigned max_temps;
   bool f16; /* USSE F16 and C10 arithmetic */
};

/* the features of the core sgxws_dev_info describes, or NULL */
const struct sgx_device_features *
sgx_device_features(const struct sgxws_device_info *info);

#endif
