/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_SCREEN_H
#define SGX_SCREEN_H

#include "pipe/p_screen.h"
#include "util/simple_mtx.h"
#include "util/slab.h"

#include "sgxws.h"

/* SGX_DEBUG=flag,flag... (sgx_drv_debug_options in sgx_screen.c) */
#define SGX_DEBUG_FRAME          (1 << 0)
#define SGX_DEBUG_NIR            (1 << 1)
#define SGX_DEBUG_DRAW           (1 << 2)
#define SGX_DEBUG_SYNC           (1 << 3)
#define SGX_DEBUG_STATS          (1 << 4)
#define SGX_DEBUG_SERIAL         (1 << 5)
#define SGX_DEBUG_HANG_ABORT     (1 << 6)
#define SGX_DEBUG_FLUSH_ALL      (1 << 7)
#define SGX_DEBUG_NO_VCACHE      (1 << 8)
#define SGX_DEBUG_NO_F16         (1 << 9)
#define SGX_DEBUG_NO_DRV_CONSTS  (1 << 10)
#define SGX_DEBUG_FS_CONST       (1 << 11)
#define SGX_DEBUG_ATTRS          (1 << 12)
#define SGX_DEBUG_BLITS          (1 << 13)
#define SGX_DEBUG_NO_VS_COMPILER (1 << 14)
#define SGX_DEBUG_NO_FS_COMPILER (1 << 15)
#define SGX_DEBUG_FLUSH_RT       (1 << 16)
#define SGX_DEBUG_FF_MEDIUMP     (1 << 17)
#define SGX_DEBUG_ZERO_COPY      (1 << 18)

extern uint32_t sgx_drv_debug;
/* SGX_DUMP_CODE=dir: every compiled program's instructions written there */
const char *sgx_dump_code_dir(void);

#define SGX_DBG(flag, ...)                                                     \
   do {                                                                        \
      if (sgx_drv_debug & SGX_DEBUG_##flag)                                    \
         fprintf(stderr, "sgx: " __VA_ARGS__);                                 \
   } while (0)

struct sgx_device_features;

struct sgx_screen {
   struct pipe_screen base;
   int fd;     /* the fd the screen was created on (its key) */
   int dev_fd; /* the fd the services connection lives on */

   /* the services connection; one per screen, used under lock */
   struct sgxws_dev *dev;
   simple_mtx_t lock;
   /* The one hardware render context (CCB, parameter buffer) of the process,
    * shared by every pipe context and serialised by the lock, as the DDK runs a
    * GL client on one render context: a second render context with a parameter
    * buffer of its own makes the microkernel's renders slow and then lock up
    * (an application with two GL contexts).  A frame being
    * recorded is flushed when another target or context starts one
    * (sgxws_begin). */
   struct sgxws_ctx *hw;

   struct slab_parent_pool transfer_pool;

   /* buffers' device copies a render may still read: freed once it's done */
   struct {
      struct sgxws_bo bo;
      uint32_t seq;
   } retired[32];
   unsigned n_retired;

   /* kmsro: the display device scanout buffers are allocated on */
   struct renderonly *ro;

   /* the core: what the kernel reports, and what its entry adds */
   const struct sgxws_device_info *info;
   const struct sgx_device_features *features;
   char name[48];
};

static inline struct sgx_screen *
sgx_screen(struct pipe_screen *pscreen)
{
   return (struct sgx_screen *)pscreen;
}

struct renderonly;

/* ro: a KMS-only display device that scanout buffers come from (kmsro), or NULL
 */
struct pipe_screen *sgx_screen_create(int fd,
                                      const struct pipe_screen_config *config,
                                      struct renderonly *ro);
void sgx_fence_screen_init(struct sgx_screen *screen);

#endif
