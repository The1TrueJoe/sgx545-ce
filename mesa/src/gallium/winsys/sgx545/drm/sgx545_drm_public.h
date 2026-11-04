/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX545_DRM_PUBLIC_H
#define SGX545_DRM_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;
struct renderonly;

/* the GPU's render node paired with a KMS-only display device (kmsro) */
struct pipe_screen *sgx545_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                                        const struct pipe_screen_config *config);

#endif
