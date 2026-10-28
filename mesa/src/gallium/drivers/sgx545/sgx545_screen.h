/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX545_SCREEN_H
#define SGX545_SCREEN_H

#include "pipe/p_screen.h"
#include "util/simple_mtx.h"
#include "util/slab.h"

#include "sgx545_frame.h"

#define SGX545_DEBUG_FRAME (1 << 0)
#define SGX545_DEBUG_NIR   (1 << 1)
#define SGX545_DEBUG_DRAW  (1 << 2)
#define SGX545_DEBUG_SYNC  (1 << 3)

extern uint32_t sgx545_debug;

#define SGX545_DBG(flag, ...)                                                  \
   do {                                                                        \
      if (sgx545_debug & SGX545_DEBUG_##flag)                                  \
         fprintf(stderr, "sgx545: " __VA_ARGS__);                              \
   } while (0)

struct sgx545_screen {
   struct pipe_screen base;
   int fd;

   /* the services connection; one per screen, used under lock */
   struct sgxws_dev dev;
   simple_mtx_t lock;

   struct slab_parent_pool transfer_pool;
};

static inline struct sgx545_screen *
sgx545_screen(struct pipe_screen *pscreen)
{
   return (struct sgx545_screen *)pscreen;
}

struct pipe_screen *sgx545_screen_create(int fd, const struct pipe_screen_config *config);
void sgx545_fence_screen_init(struct sgx545_screen *screen);

#endif
