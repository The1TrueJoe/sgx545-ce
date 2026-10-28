/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include "util/os_file.h"
#include "util/u_screen.h"

#include "sgx545/sgx545_public.h"
#include "sgx545/sgx545_screen.h"

static struct pipe_screen *
create_screen(int fd, const struct pipe_screen_config *config, struct renderonly *ro)
{
   return sgx545_screen_create(fd, config);
}

/* fd: the services DRM node (pvrsrvkm); the screen keeps a dup of it */
struct pipe_screen *
sgx545_drm_screen_create(int fd, const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, NULL, create_screen);
}
