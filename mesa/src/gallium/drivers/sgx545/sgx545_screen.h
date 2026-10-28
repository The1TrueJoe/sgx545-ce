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
   int fd;                      /* the fd the screen was created on (its key) */
   int dev_fd;                  /* the fd the services connection lives on */

   /* the services connection; one per screen, used under lock */
   struct sgxws_dev dev;
   simple_mtx_t lock;
   /* The one hardware render context (CCB, parameter buffer) of the process,
    * shared by every pipe context and serialised by the lock, as the DDK runs a
    * GL client on one render context: a second render context with a parameter
    * buffer of its own makes the microkernel's renders slow and then lock up
    * (gl4es under glretrace, which creates two pipe contexts).  A frame being
    * recorded is flushed when another target or context starts one
    * (sgxws_begin). */
   struct sgxws_ctx *hw;

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
