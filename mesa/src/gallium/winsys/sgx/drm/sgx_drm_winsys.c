/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include "util/os_file.h"
#include "util/u_screen.h"

#include "sgx/sgx_public.h"
#include "sgx/sgx_screen.h"
#include "sgx_drm_public.h"

static struct pipe_screen *
create_screen(int fd, const struct pipe_screen_config *config,
              struct renderonly *ro)
{
   return sgx_screen_create(fd, config, ro);
}

/* fd: the GPU's node (powervr_sgx, or pvrsrvkm); the screen keeps a dup of it */
struct pipe_screen *
sgx_drm_screen_create(int fd, const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, NULL,
                                         create_screen);
}

/* fd: powervr_sgx's render node; scanout buffers come from ro's display device */
struct pipe_screen *
sgx_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                 const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, ro,
                                         create_screen);
}
