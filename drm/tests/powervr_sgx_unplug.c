// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * Unbind with a file and a CPU mapping open, then bind again (drm-uapi.rst,
 * "Device Hot-Unplug"):
 *
 *   powervr_sgx_unplug
 *
 * Maps a buffer, fills it, unbinds powervr_sgx from its device through sysfs,
 * and checks that the mapping can still be read and written without a signal
 * and that ioctls on the old file fail with ENODEV. Then it closes everything,
 * binds the driver again and checks the new device answers.
 *
 * Needs root. Exit 0 on success, 1 on a failure, 77 when there is no
 * powervr_sgx device.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <drm/drm.h>
#include "powervr_sgx_drm.h"

#define BO_SIZE (64 * 1024)

static sigjmp_buf jb;
static volatile sig_atomic_t sig;

static void on_signal(int s)
{
	sig = s;
	siglongjmp(jb, 1);
}

static int open_dev(void)
{
	DIR *d = opendir("/dev/dri");
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char path[300], name[32] = "";
		struct drm_version v = { .name_len = sizeof(name) - 1, .name = name };
		int fd;

		if (strncmp(e->d_name, "renderD", 7))
			continue;
		snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "powervr_sgx")) {
			closedir(d);
			return fd;
		}
		close(fd);
	}
	closedir(d);
	return -1;
}

static int write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC), r;

	if (fd < 0)
		return -errno;
	r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -errno;
	close(fd);
	return r;
}

static int query_size(int fd)
{
	struct drm_powervr_sgx_ioctl_dev_query_args q = {
		.type = DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET,
	};

	return ioctl(fd, DRM_IOCTL_POWERVR_SGX_DEV_QUERY, &q) ? -errno : 0;
}

int main(void)
{
	struct drm_powervr_sgx_ioctl_create_bo_args bo = {
		.size = BO_SIZE, .flags = DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS,
	};
	struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args mo = {};
	char link[PATH_MAX], drv[PATH_MAX], path[PATH_MAX + 8], *dev;
	volatile uint32_t *p;
	struct stat st;
	int fd, err, fails = 0;
	uint32_t sum = 0;

	fd = open_dev();
	if (fd < 0) {
		printf("SKIP: no powervr_sgx render node\n");
		return 77;
	}
	if (fstat(fd, &st))
		return 1;
	snprintf(link, sizeof(link), "/sys/dev/char/%u:%u/device", major(st.st_rdev),
		 minor(st.st_rdev));
	if (!realpath(link, link)) {
		printf("FAIL: %s: %s\n", link, strerror(errno));
		return 1;
	}
	dev = strrchr(link, '/') + 1;
	snprintf(drv, sizeof(drv), "%s/driver", link);
	if (!realpath(drv, drv)) {
		printf("FAIL: %s: %s\n", drv, strerror(errno));
		return 1;
	}

	if (ioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &bo) ||
	    (mo.handle = bo.handle, ioctl(fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &mo))) {
		printf("FAIL: create and map a BO: %s\n", strerror(errno));
		return 1;
	}
	p = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mo.offset);
	if (p == MAP_FAILED) {
		printf("FAIL: mmap: %s\n", strerror(errno));
		return 1;
	}
	for (unsigned int i = 0; i < BO_SIZE / 4; i++)
		p[i] = i;

	printf("unbind %s from %s\n", dev, drv);
	snprintf(path, sizeof(path), "%s/unbind", drv);
	err = write_file(path, dev);
	if (err) {
		printf("FAIL: unbind: %s\n", strerror(-err));
		return 1;
	}

	signal(SIGBUS, on_signal);
	signal(SIGSEGV, on_signal);
	if (!sigsetjmp(jb, 1)) {
		for (unsigned int i = 0; i < BO_SIZE / 4; i++)
			sum += p[i];
		for (unsigned int i = 0; i < BO_SIZE / 4; i++)
			p[i] = ~i;
		printf("ok: the mapping reads and writes after unbind (sum 0x%x)\n", sum);
	} else {
		printf("FAIL: signal %d touching the mapping after unbind\n", (int)sig);
		fails++;
	}
	signal(SIGBUS, SIG_DFL);
	signal(SIGSEGV, SIG_DFL);

	err = query_size(fd);
	if (err == -ENODEV) {
		printf("ok: DEV_QUERY on the old file: ENODEV\n");
	} else {
		printf("FAIL: DEV_QUERY on the old file returned %d, expected -ENODEV\n", err);
		fails++;
	}
	bo.handle = 0;
	err = ioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &bo) ? -errno : 0;
	if (err == -ENODEV) {
		printf("ok: CREATE_BO on the old file: ENODEV\n");
	} else {
		printf("FAIL: CREATE_BO on the old file returned %d, expected -ENODEV\n", err);
		fails++;
	}

	munmap((void *)p, BO_SIZE);
	close(fd);

	snprintf(path, sizeof(path), "%s/bind", drv);
	err = write_file(path, dev);
	if (err) {
		printf("FAIL: bind: %s\n", strerror(-err));
		return 1;
	}
	for (int i = 0; i < 50 && (fd = open_dev()) < 0; i++)
		usleep(100 * 1000);
	if (fd < 0 || query_size(fd)) {
		printf("FAIL: no working device after bind\n");
		return 1;
	}
	printf("ok: bound again, the new device answers\n");
	close(fd);

	printf("%s\n", fails ? "FAILED" : "all right");
	return fails ? 1 : 0;
}
