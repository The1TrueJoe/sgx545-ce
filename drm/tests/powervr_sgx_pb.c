// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * Set the size of the device's parameter buffer before any client does:
 *
 *   powervr_sgx_pb PAGES
 *
 * The first render context after the module loads sets the size of the PB that
 * every later context shares (until the module is unloaded); later requests are
 * ignored. Run it before Xorg or any GL client, for example to make a scene run
 * out of parameter memory sooner. The kernel logs the size it used.
 *
 * Exit 0 when the context was created; 77 when there is no powervr_sgx device.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* musl's <sys/ioctl.h> redefines the kernel's _IOC: declare ioctl alone. */
int ioctl(int fd, unsigned long req, ...);

#include <drm.h>

#include "powervr_sgx_drm.h"

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r == -1 ? -errno : 0;
}

int main(int argc, char **argv)
{
	struct drm_powervr_sgx_ioctl_create_vm_context_args vm = {};
	struct drm_powervr_sgx_ioctl_create_context_args cc = {
		.type = DRM_POWERVR_SGX_CTX_TYPE_RENDER,
	};
	int fd, e;

	if (argc != 2) {
		fprintf(stderr, "usage: powervr_sgx_pb PAGES\n");
		return 2;
	}
	cc.param_buffer_pages = (uint32_t)strtoul(argv[1], NULL, 0);
	fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return 77;
	if ((e = xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT, &vm))) {
		fprintf(stderr, "powervr_sgx_pb: CREATE_VM_CONTEXT: %d\n", e);
		return 1;
	}
	cc.vm_context_handle = vm.handle;
	if ((e = xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &cc))) {
		fprintf(stderr, "powervr_sgx_pb: CREATE_CONTEXT with %#x pages: %d\n",
			cc.param_buffer_pages, e);
		return 1;
	}
	printf("render context with %#x PB pages; the kernel log says the size in use\n",
	       cc.param_buffer_pages);
	close(fd);
	return 0;
}
