/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_DRM_PUBLIC_H
#define SGX_DRM_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;
struct renderonly;

/* the GPU's render node paired with a KMS-only display device (kmsro) */
struct pipe_screen *sgx_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                                        const struct pipe_screen_config *config);

#endif
