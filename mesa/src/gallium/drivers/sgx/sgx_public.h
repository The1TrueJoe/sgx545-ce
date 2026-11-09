/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_PUBLIC_H
#define SGX_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;

struct pipe_screen *
sgx_drm_screen_create(int fd, const struct pipe_screen_config *config);

#endif
