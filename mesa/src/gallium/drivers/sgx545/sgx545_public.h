/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX545_PUBLIC_H
#define SGX545_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;

struct pipe_screen *sgx545_drm_screen_create(int fd, const struct pipe_screen_config *config);

#endif
