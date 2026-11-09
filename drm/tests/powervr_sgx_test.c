// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * uAPI tests for powervr_sgx, one subtest per behaviour, IGT style: the
 * subtests and their names map one to one onto an IGT tests/powervr_sgx/
 * suite once the driver is submitted. Until then this is a static binary
 * with a small harness, so it runs on an EA with nothing installed.
 *
 *   powervr_sgx_test                 run everything
 *   powervr_sgx_test -l              list subtests
 *   powervr_sgx_test <glob>...       run matching subtests
 *
 * Each subtest runs in its own child on a fresh fd; exit status is the
 * number of failures.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Not <sys/ioctl.h>: musl redefines the kernel's _IOC macros there. */
int ioctl(int fd, unsigned long req, ...);

#include <drm/drm.h>
#include "powervr_sgx_drm.h"

#define PAGE 4096u

#define DRM_POWERVR_SGX_OBJ_ARRAY(cnt, ptr) \
	{ .stride = sizeof((ptr)[0]), .count = (cnt), .array = (__u64)(uintptr_t)(ptr) }

static const char *cur;

#define fail(...) do { \
	fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define assert_true(c) do { if (!(c)) fail("%s", #c); } while (0)
#define assert_eq(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
	if (_a != _b) fail("%s == %lld, expected %s == %lld", #a, _a, #b, _b); } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r == -1 ? -errno : 0;
}

#define do_ioctl(fd, req, arg) do { int _e = xioctl(fd, req, arg); \
	if (_e) fail("%s: %s", #req, strerror(-_e)); } while (0)
#define do_ioctl_err(fd, req, arg, err) do { int _e = xioctl(fd, req, arg); \
	if (_e != -(err)) fail("%s returned %d (%s), expected -%s", #req, _e, \
			       _e ? strerror(-_e) : "ok", #err); } while (0)

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

/* helpers */

static uint32_t create_bo(int fd, uint64_t size, uint64_t flags)
{
	struct drm_powervr_sgx_ioctl_create_bo_args a = { .size = size, .flags = flags };

	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a);
	assert_true(a.handle);
	return a.handle;
}

static uint32_t create_vm(int fd)
{
	struct drm_powervr_sgx_ioctl_create_vm_context_args a = {};

	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT, &a);
	assert_true(a.handle);
	return a.handle;
}

static int vm_map(int fd, uint32_t vm, uint32_t bo, uint64_t addr, uint64_t off, uint64_t size)
{
	struct drm_powervr_sgx_ioctl_vm_map_args a = {
		.vm_context_handle = vm, .device_addr = addr, .handle = bo, .offset = off, .size = size,
	};

	return xioctl(fd, DRM_IOCTL_POWERVR_SGX_VM_MAP, &a);
}

static int vm_unmap(int fd, uint32_t vm, uint64_t addr, uint64_t size)
{
	struct drm_powervr_sgx_ioctl_vm_unmap_args a = {
		.vm_context_handle = vm, .device_addr = addr, .size = size,
	};

	return xioctl(fd, DRM_IOCTL_POWERVR_SGX_VM_UNMAP, &a);
}

static uint32_t create_ctx(int fd, uint32_t vm, uint32_t type)
{
	struct drm_powervr_sgx_ioctl_create_context_args a = {
		.type = type, .vm_context_handle = vm,
	};

	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &a);
	assert_true(a.handle);
	return a.handle;
}

static uint32_t syncobj(int fd)
{
	struct drm_syncobj_create a = {};

	do_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &a);
	return a.handle;
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static int syncobj_wait(int fd, uint32_t h, int64_t timeout_ns)
{
	struct drm_syncobj_wait w = {
		.handles = (uintptr_t)&h, .count_handles = 1,
		.timeout_nsec = now_ns() + timeout_ns,
	};

	return xioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

static int syncobj_timeline_wait(int fd, uint32_t h, uint64_t point, int64_t timeout_ns)
{
	struct drm_syncobj_timeline_wait w = {
		.handles = (uintptr_t)&h, .points = (uintptr_t)&point, .count_handles = 1,
		.timeout_nsec = now_ns() + timeout_ns,
	};

	return xioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w);
}

static int submit(int fd, struct drm_powervr_sgx_job *jobs, uint32_t n, uint32_t *failed)
{
	struct drm_powervr_sgx_ioctl_submit_jobs_args a = {
		.jobs = DRM_POWERVR_SGX_OBJ_ARRAY(n, jobs),
	};
	int r = xioctl(fd, DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &a);

	if (failed)
		*failed = a.failed_index;
	return r;
}

static struct drm_powervr_sgx_job null_job(uint32_t ctx, struct drm_powervr_sgx_sync_op *ops, uint32_t nops)
{
	return (struct drm_powervr_sgx_job){
		.type = DRM_POWERVR_SGX_JOB_TYPE_NULL,
		.context_handle = ctx,
		.sync_ops = DRM_POWERVR_SGX_OBJ_ARRAY(nops, ops),
	};
}

#define SIGNAL(h) { .handle = (h), .flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL }
#define WAIT(h) { .handle = (h), .flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_WAIT }

/* DEV_QUERY */

static int query(int fd, uint32_t type, void *p, uint32_t *size)
{
	struct drm_powervr_sgx_ioctl_dev_query_args a = {
		.type = type, .size = *size, .pointer = (uintptr_t)p,
	};
	int r = xioctl(fd, DRM_IOCTL_POWERVR_SGX_DEV_QUERY, &a);

	*size = a.size;
	return r;
}

static void t_query_gpu_info(int fd)
{
	struct drm_powervr_sgx_dev_query_gpu_info i;
	uint32_t size = 0;

	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET, NULL, &size), 0);
	assert_eq(size, sizeof(i));
	memset(&i, 0xff, sizeof(i));
	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET, &i, &size), 0);
	printf("  core_id 0x%08x core_revision 0x%08x fw_interface %u\n",
	       i.core_id, i.core_revision, i.fw_interface_version);
	assert_eq(i.core_id >> 16, 0x0115);		/* SGX545 */
	/* The cores in the driver's table: 1.0.14 (CE5300) or 1.0.13 (Cedarview). */
	assert_true((i.core_revision & 0xffffff) == 0x1000e ||
		    (i.core_revision & 0xffffff) == 0x1000d);
	assert_eq(i.fw_interface_version, DRM_POWERVR_SGX_FW_IF_SGXMKIF_1_7);
	assert_eq(i._padding_c, 0);
}

static void t_query_short(int fd)
{
	uint32_t buf[4] = { ~0u, ~0u, ~0u, ~0u }, size = 8;

	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET, buf, &size), 0);
	assert_eq(size, 8);
	assert_eq(buf[2], ~0u);	/* nothing written past the caller's size */
}

static void t_query_limits_quirks(int fd)
{
	struct drm_powervr_sgx_dev_query_limits l = {};
	struct drm_powervr_sgx_dev_query_quirks q = {};
	uint32_t size = sizeof(l);

	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_LIMITS_GET, &l, &size), 0);
	assert_true(l.rt_max_width && l.rt_max_height && l.rt_max_samples == 1);
	assert_true(l.pb_min_pages <= l.pb_default_pages && l.pb_default_pages <= l.pb_max_pages);
	size = sizeof(q);
	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET, &q, &size), 0);
	assert_eq(q.count, 0);
	q._padding_c = 1;
	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET, &q, &size), -EINVAL);
}

static void t_query_heap_info(int fd)
{
	struct drm_powervr_sgx_heap h[DRM_POWERVR_SGX_HEAP_COUNT + 2];
	struct drm_powervr_sgx_dev_query_heap_info info = {};
	uint32_t size = sizeof(info);

	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET, &info, &size), 0);
	assert_eq(info.heaps.count, DRM_POWERVR_SGX_HEAP_COUNT);
	assert_eq(info.heaps.stride, sizeof(h[0]));

	memset(h, 0xa5, sizeof(h));
	info.heaps = (struct drm_powervr_sgx_obj_array)DRM_POWERVR_SGX_OBJ_ARRAY(
		DRM_POWERVR_SGX_HEAP_COUNT + 2, h);
	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET, &info, &size), 0);
	assert_eq(info.heaps.count, DRM_POWERVR_SGX_HEAP_COUNT);
	assert_eq(h[DRM_POWERVR_SGX_HEAP_GENERAL].base, 0x3e000000);
	assert_eq(h[DRM_POWERVR_SGX_HEAP_USSE_PIXEL].base, 0x37000000);
	assert_eq(h[DRM_POWERVR_SGX_HEAP_USSE_PIXEL].usse_window_log2, 19);
	assert_eq(h[DRM_POWERVR_SGX_HEAP_USSE_VERTEX].usse_base_index, 10);
	for (int i = 0; i < DRM_POWERVR_SGX_HEAP_COUNT; i++) {
		assert_true(h[i].size && h[i].base + h[i].size <= (1ull << 32));
		assert_eq(h[i].page_size_log2, 12);
		/* No client heap reaches the kernel's ranges. */
		assert_true(h[i].base + h[i].size <= 0x30000000 || h[i].base >= 0x30400000);
		assert_true(h[i].base + h[i].size <= 0x32000000 || h[i].base >= 0x37000000);
	}
	assert_eq(*(uint8_t *)&h[DRM_POWERVR_SGX_HEAP_COUNT], 0xa5);
}

static void t_query_invalid(int fd)
{
	uint32_t size = 16;
	struct drm_powervr_sgx_dev_query_gpu_info i;

	assert_eq(query(fd, 99, &i, &size), -EINVAL);
	size = sizeof(i);
	assert_eq(query(fd, DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET, (void *)8, &size), -EFAULT);
}

/* CREATE_BO, GET_BO_MMAP_OFFSET */

static void t_create_bo(int fd)
{
	struct drm_gem_close c = { .handle = create_bo(fd, PAGE, 0) };

	do_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
	create_bo(fd, 16 * PAGE, DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS);
}

static void t_create_bo_invalid(int fd)
{
	struct drm_powervr_sgx_ioctl_create_bo_args a = {};

	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a, EINVAL);
	a.size = 100;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a, EINVAL);
	a.size = PAGE;
	a.flags = 1ull << 7;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a, EINVAL);
	a.flags = 0;
	a._padding_c = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a, EINVAL);
	a._padding_c = 0;
	a.size = 1ull << 33;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a, EINVAL);
}

static void t_mmap_bo(int fd)
{
	struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args a = {
		.handle = create_bo(fd, 4 * PAGE, DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS),
	};
	uint32_t *p;

	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &a);
	p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, a.offset);
	assert_true(p != MAP_FAILED);
	for (unsigned int i = 0; i < 4 * PAGE / 4; i++)
		assert_eq(p[i], 0);	/* new objects are zeroed */
	for (unsigned int i = 0; i < 4 * PAGE / 4; i++)
		p[i] = i * 2654435761u;
	for (unsigned int i = 0; i < 4 * PAGE / 4; i++)
		assert_eq(p[i], (uint32_t)(i * 2654435761u));
	munmap(p, 4 * PAGE);
}

static void t_mmap_bo_invalid(int fd)
{
	struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args a = { .handle = create_bo(fd, PAGE, 0) };

	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &a, EINVAL);
	a.handle = 0xdead;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &a, ENOENT);
	a.handle = create_bo(fd, PAGE, DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS);
	a._padding_4 = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &a, EINVAL);
}

/* VM contexts, VM_MAP, VM_UNMAP */

static void t_vm_create_destroy(int fd)
{
	struct drm_powervr_sgx_ioctl_destroy_vm_context_args d = { .handle = create_vm(fd) };

	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT, &d);
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT, &d, EINVAL);
	d.handle = 0;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT, &d, EINVAL);
	d.handle = create_vm(fd);
	d._padding_4 = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT, &d, EINVAL);
}

static void t_vm_map_unmap(int fd)
{
	uint32_t vm = create_vm(fd), bo = create_bo(fd, 8 * PAGE, 0);

	assert_eq(vm_map(fd, vm, bo, 0x3e000000, 0, 8 * PAGE), 0);
	assert_eq(vm_map(fd, vm, bo, 0x3e100000, 4 * PAGE, 4 * PAGE), 0);
	assert_eq(vm_map(fd, vm, bo, 0x37000000, 0, PAGE), -EINVAL);	/* USSE pixel: code objects only */
	assert_eq(vm_map(fd, vm, bo, 0xfffff000, 0, PAGE), 0);	/* last page */
	assert_eq(vm_unmap(fd, vm, 0x3e000000, 8 * PAGE), 0);
	assert_eq(vm_unmap(fd, vm, 0x3e000000, 8 * PAGE), -ENOENT);
	assert_eq(vm_unmap(fd, vm, 0x3e100000, PAGE), -ENOENT);	/* not the whole mapping */
	assert_eq(vm_unmap(fd, vm, 0x3e100000, 4 * PAGE), 0);
	/* the range is free again */
	assert_eq(vm_map(fd, vm, bo, 0x3e000000, 0, 8 * PAGE), 0);
}

static void t_vm_map_overlap(int fd)
{
	uint32_t vm = create_vm(fd), bo = create_bo(fd, 8 * PAGE, 0);

	assert_eq(vm_map(fd, vm, bo, 0x3e000000, 0, 4 * PAGE), 0);
	assert_eq(vm_map(fd, vm, bo, 0x3e003000, 0, 4 * PAGE), -EEXIST);
	assert_eq(vm_map(fd, vm, bo, 0x3dfff000, 0, 2 * PAGE), -EINVAL);	/* also crosses a heap */
	assert_eq(vm_map(fd, vm, bo, 0x3e004000, 0, 4 * PAGE), 0);
	/* a second VM is independent */
	assert_eq(vm_map(fd, create_vm(fd), bo, 0x3e000000, 0, 4 * PAGE), 0);
}

static void t_vm_map_kernel_ranges(int fd)
{
	uint32_t vm = create_vm(fd), bo = create_bo(fd, 2 * PAGE, 0);
	static const uint64_t forbidden[] = {
		0x00000000, 0x10000000, 0x1ffff000, 0x20000000, 0x2d000000,
		0x30000000, 0x303ff000, 0x32000000, 0x32003000, 0x36fff000,
		0x37500000, 0x3c200000, 0x100000000ull,
	};

	for (unsigned int i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++)
		if (vm_map(fd, vm, bo, forbidden[i], 0, PAGE) != -EINVAL)
			fail("mapped at reserved 0x%llx", (unsigned long long)forbidden[i]);
	/* straddling the end of the PDS pixel heap into the microkernel's */
	assert_eq(vm_map(fd, vm, bo, 0x2ffff000, 0, 2 * PAGE), -EINVAL);
	/* straddling the end of the USSE pixel heap */
	assert_eq(vm_map(fd, vm, bo, 0x374ff000, 0, 2 * PAGE), -EINVAL);
}

static void t_vm_map_invalid(int fd)
{
	uint32_t vm = create_vm(fd), bo = create_bo(fd, 4 * PAGE, 0);
	struct drm_powervr_sgx_ioctl_vm_map_args a = {
		.vm_context_handle = vm, .device_addr = 0x3e000000, .handle = bo, .size = PAGE,
	};

	assert_eq(vm_map(fd, vm, bo, 0x3e000800, 0, PAGE), -EINVAL);
	assert_eq(vm_map(fd, vm, bo, 0x3e000000, 0x800, PAGE), -EINVAL);
	assert_eq(vm_map(fd, vm, bo, 0x3e000000, 0, 0x800), -EINVAL);
	assert_eq(vm_map(fd, vm, bo, 0x3e000000, 0, 0), -EINVAL);
	assert_eq(vm_map(fd, vm, bo, 0x3e000000, 2 * PAGE, 4 * PAGE), -EINVAL);
	assert_eq(vm_map(fd, vm, bo, 0x3e000000, ~0ull & ~0xfffull, 2 * PAGE), -EINVAL);
	assert_eq(vm_map(fd, vm + 100, bo, 0x3e000000, 0, PAGE), -EINVAL);
	assert_eq(vm_map(fd, vm, 0xdead, 0x3e000000, 0, PAGE), -ENOENT);
	a.flags = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_VM_MAP, &a, EINVAL);
	a.flags = 0;
	a._padding_14 = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_VM_MAP, &a, EINVAL);
}

static void t_vm_handles_per_file(int fd)
{
	uint32_t vm = create_vm(fd);
	int fd2 = open_dev();

	assert_true(fd2 >= 0);
	assert_eq(vm_map(fd2, vm, create_bo(fd2, PAGE, 0), 0x3e000000, 0, PAGE), -EINVAL);
	close(fd2);
}

/* Contexts */

static void t_context_create_destroy(int fd)
{
	uint32_t vm = create_vm(fd);
	struct drm_powervr_sgx_ioctl_destroy_context_args d = {
		.handle = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER),
	};
	struct drm_powervr_sgx_ioctl_get_context_state_args s = { .handle = d.handle };

	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE, &s);
	assert_eq(s.state, DRM_POWERVR_SGX_CONTEXT_STATE_OK);
	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT, &d);
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT, &d, EINVAL);
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE, &s, EINVAL);
	create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
}

static void t_context_invalid(int fd)
{
	uint32_t vm = create_vm(fd);
	struct drm_powervr_sgx_ioctl_create_context_args a = { .vm_context_handle = vm };

	a.type = 7;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &a, EINVAL);
	a.type = DRM_POWERVR_SGX_CTX_TYPE_RENDER;
	a.flags = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &a, EINVAL);
	a.flags = 0;
	a.priority = 3;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &a, EINVAL);
	a.priority = 0;
	a.vm_context_handle = vm + 100;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &a, EINVAL);
	a.vm_context_handle = vm;
	a.param_buffer_pages = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &a, EINVAL);
	a.type = DRM_POWERVR_SGX_CTX_TYPE_TRANSFER;
	a.param_buffer_pages = 0x400;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &a, EINVAL);
}

/* SUBMIT_JOBS */

static void t_null_job(int fd)
{
	uint32_t ctx = create_ctx(fd, create_vm(fd), DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t s = syncobj(fd);
	struct drm_powervr_sgx_sync_op op = SIGNAL(s);
	struct drm_powervr_sgx_job j = null_job(ctx, &op, 1);
	int64_t t0 = now_ns();

	assert_eq(submit(fd, &j, 1, NULL), 0);
	assert_eq(syncobj_wait(fd, s, 2000000000ll), 0);
	printf("  null job signalled in %lld us\n", (long long)(now_ns() - t0) / 1000);
}

static void t_null_job_timeline(int fd)
{
	uint32_t ctx = create_ctx(fd, create_vm(fd), DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
	uint32_t s = syncobj(fd);
	struct drm_powervr_sgx_sync_op op = {
		.handle = s, .value = 5,
		.flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL |
			 DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ,
	};
	struct drm_powervr_sgx_job j = null_job(ctx, &op, 1);

	assert_eq(submit(fd, &j, 1, NULL), 0);
	assert_eq(syncobj_timeline_wait(fd, s, 5, 2000000000ll), 0);
}

/* A waits on nothing, B waits on A's out-fence from another context, C on B. */
static void t_null_job_chain(int fd)
{
	uint32_t vm = create_vm(fd);
	uint32_t c1 = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t c2 = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
	uint32_t a = syncobj(fd), b = syncobj(fd), c = syncobj(fd);
	struct drm_powervr_sgx_sync_op oa[] = { SIGNAL(a) };
	struct drm_powervr_sgx_sync_op ob[] = { WAIT(a), SIGNAL(b) };
	struct drm_powervr_sgx_sync_op oc[] = { WAIT(b), SIGNAL(c) };
	struct drm_powervr_sgx_job j[] = {
		null_job(c1, oa, 1), null_job(c2, ob, 2), null_job(c1, oc, 2),
	};

	assert_eq(submit(fd, j, 3, NULL), 0);
	assert_eq(syncobj_wait(fd, c, 2000000000ll), 0);
	assert_eq(syncobj_wait(fd, a, 0), 0);
	assert_eq(syncobj_wait(fd, b, 0), 0);
}

static void t_null_job_many(int fd)
{
	uint32_t ctx = create_ctx(fd, create_vm(fd), DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t s = syncobj(fd);
	struct drm_powervr_sgx_sync_op op = SIGNAL(s);
	int64_t t0 = now_ns();

	/* More than the 256-slot kernel CCB holds. */
	for (int i = 0; i < 600; i++) {
		struct drm_powervr_sgx_job j = null_job(ctx, &op, 1);

		assert_eq(submit(fd, &j, 1, NULL), 0);
	}
	assert_eq(syncobj_wait(fd, s, 10000000000ll), 0);
	printf("  600 null jobs in %lld ms\n", (long long)(now_ns() - t0) / 1000000);
}

static void t_null_job_bo_ref(int fd)
{
	uint32_t vm = create_vm(fd), bo = create_bo(fd, PAGE, 0);
	uint32_t ctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER), s = syncobj(fd);
	struct drm_powervr_sgx_sync_op op = SIGNAL(s);
	struct drm_powervr_sgx_bo_ref refs[] = {
		{ .handle = bo, .flags = DRM_POWERVR_SGX_BO_REF_FLAG_WRITE }, { .handle = bo },
	};
	struct drm_powervr_sgx_job j = null_job(ctx, &op, 1);

	j.bo_refs = (struct drm_powervr_sgx_obj_array)DRM_POWERVR_SGX_OBJ_ARRAY(2, refs);
	assert_eq(submit(fd, &j, 1, NULL), 0);
	assert_eq(syncobj_wait(fd, s, 2000000000ll), 0);
	refs[0].flags = 2;
	assert_eq(submit(fd, &j, 1, NULL), -EINVAL);
	refs[0].flags = 0;
	refs[1].handle = 0xdead;
	assert_eq(submit(fd, &j, 1, NULL), -ENOENT);
}

/* The VM goes away while a context still runs jobs in it. */
static void t_null_job_vm_destroyed(int fd)
{
	struct drm_powervr_sgx_ioctl_destroy_vm_context_args d = { .handle = create_vm(fd) };
	uint32_t ctx = create_ctx(fd, d.handle, DRM_POWERVR_SGX_CTX_TYPE_RENDER), s = syncobj(fd);
	struct drm_powervr_sgx_sync_op op = SIGNAL(s);
	struct drm_powervr_sgx_job j = null_job(ctx, &op, 1);

	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT, &d);
	assert_eq(submit(fd, &j, 1, NULL), 0);
	assert_eq(syncobj_wait(fd, s, 2000000000ll), 0);
}

static void t_submit_invalid(int fd)
{
	uint32_t ctx = create_ctx(fd, create_vm(fd), DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t s = syncobj(fd), failed;
	struct drm_powervr_sgx_sync_op op = SIGNAL(s);
	struct drm_powervr_sgx_job j[2] = { null_job(ctx, NULL, 0), null_job(ctx, &op, 1) };
	struct drm_powervr_sgx_ioctl_submit_jobs_args a = {};

	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &a, EINVAL);	/* no jobs */
	a.jobs = (struct drm_powervr_sgx_obj_array)DRM_POWERVR_SGX_OBJ_ARRAY(1, j);
	a._padding_14 = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &a, EINVAL);
	a.jobs.array = 8;
	a._padding_14 = 0;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &a, EFAULT);

	j[1].type = DRM_POWERVR_SGX_JOB_TYPE_RENDER;	/* no RT dataset */
	assert_eq(submit(fd, j, 2, &failed), -ENOENT);
	assert_eq(failed, 1);
	j[1].type = DRM_POWERVR_SGX_JOB_TYPE_TRANSFER;	/* no stream */
	assert_eq(submit(fd, j, 2, &failed), -EINVAL);
	assert_eq(failed, 1);
	j[1].type = 9;
	assert_eq(submit(fd, j, 2, &failed), -EINVAL);
	assert_eq(failed, 1);
	j[1] = null_job(ctx, &op, 1);
	j[1].flags = 1;
	assert_eq(submit(fd, j, 2, &failed), -EINVAL);
	j[1].flags = 0;
	j[1].cmd_stream_len = 16;
	assert_eq(submit(fd, j, 2, &failed), -EINVAL);
	j[1].cmd_stream_len = 0;
	j[1].context_handle = 0xdead;
	assert_eq(submit(fd, j, 2, &failed), -ENOENT);
	assert_eq(failed, 1);
	j[1].context_handle = ctx;
	j[1]._padding_4c = 1;
	assert_eq(submit(fd, j, 2, &failed), -EINVAL);
	j[1]._padding_4c = 0;

	op.flags |= 0x100;
	assert_eq(submit(fd, j, 2, NULL), -EINVAL);
	op = (struct drm_powervr_sgx_sync_op)SIGNAL(s);
	op.value = 3;	/* a binary syncobj has no points */
	assert_eq(submit(fd, j, 2, NULL), -EINVAL);
	op = (struct drm_powervr_sgx_sync_op)SIGNAL(0xdead);
	assert_eq(submit(fd, j, 2, NULL), -ENOENT);
	op = (struct drm_powervr_sgx_sync_op)WAIT(0xdead);
	assert_eq(submit(fd, j, 2, NULL), -ENOENT);

	/* Nothing above was queued: the syncobj never got a fence. */
	assert_eq(syncobj_wait(fd, s, 0), -EINVAL);
}

static void t_context_state_invalid(int fd)
{
	struct drm_powervr_sgx_ioctl_get_context_state_args s = {
		.handle = create_ctx(fd, create_vm(fd), DRM_POWERVR_SGX_CTX_TYPE_RENDER),
	};

	s._padding_c = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE, &s, EINVAL);
	s._padding_c = 0;
	s.handle = 0xdead;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE, &s, EINVAL);
}

/* Handles are per file: another open file's context, syncobj and BO are not found. */
static void t_submit_other_file(int fd)
{
	int fd2 = open_dev();
	uint32_t ctx = create_ctx(fd, create_vm(fd), DRM_POWERVR_SGX_CTX_TYPE_RENDER), ctx2, s2, bo2;
	struct drm_powervr_sgx_sync_op op;
	struct drm_powervr_sgx_bo_ref ref;
	struct drm_powervr_sgx_job j;

	/* fd2's handles above any of fd's */
	for (int i = 0; i < 4; i++) {
		create_ctx(fd2, create_vm(fd2), DRM_POWERVR_SGX_CTX_TYPE_RENDER);
		syncobj(fd2);
		create_bo(fd2, PAGE, 0);
	}
	ctx2 = create_ctx(fd2, create_vm(fd2), DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	s2 = syncobj(fd2);
	bo2 = create_bo(fd2, PAGE, 0);
	op = (struct drm_powervr_sgx_sync_op)SIGNAL(s2);
	ref = (struct drm_powervr_sgx_bo_ref){ .handle = bo2 };

	j = null_job(ctx2, NULL, 0);
	assert_eq(submit(fd, &j, 1, NULL), -ENOENT);
	j = null_job(ctx, &op, 1);
	assert_eq(submit(fd, &j, 1, NULL), -ENOENT);
	j = null_job(ctx, NULL, 0);
	j.bo_refs = (struct drm_powervr_sgx_obj_array)DRM_POWERVR_SGX_OBJ_ARRAY(1, &ref);
	assert_eq(submit(fd, &j, 1, NULL), -ENOENT);
	close(fd2);
}

/* A wait on a syncobj that was never signalled has no fence to wait for. */
static void t_submit_wait_unsignalled(int fd)
{
	uint32_t ctx = create_ctx(fd, create_vm(fd), DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
	uint32_t s = syncobj(fd), done = syncobj(fd);
	struct drm_powervr_sgx_sync_op op[] = { WAIT(s), SIGNAL(done) };
	struct drm_powervr_sgx_job j = null_job(ctx, op, 2);

	assert_eq(submit(fd, &j, 1, NULL), -EINVAL);
	assert_eq(syncobj_wait(fd, done, 0), -EINVAL);
}

/* Code objects and UPLOAD_CODE (UAPI.md 2, D9, D12) */

#include "usse_corpus.h"

static int upload(int fd, uint32_t bo, uint64_t off, const void *w, uint64_t size, uint64_t *bad)
{
	struct drm_powervr_sgx_ioctl_upload_code_args a = {
		.handle = bo, .offset = off, .size = size, .pointer = (uintptr_t)w,
	};
	int r = xioctl(fd, DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE, &a);

	if (bad)
		*bad = a.bad_offset;
	return r;
}

static void t_code_bo(int fd)
{
	struct drm_powervr_sgx_ioctl_create_bo_args a = {
		.size = PAGE, .flags = DRM_POWERVR_SGX_BO_CODE | DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS,
	};
	struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args mo = {};
	struct drm_prime_handle ph = { .flags = DRM_CLOEXEC };
	uint32_t vm = create_vm(fd), code = create_bo(fd, 2 * PAGE, DRM_POWERVR_SGX_BO_CODE);
	uint32_t plain = create_bo(fd, PAGE, 0);

	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a, EINVAL);	/* no CPU access */
	mo.handle = code;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &mo, EINVAL);
	ph.handle = code;
	assert_true(xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0);	/* not exportable */
	ph.handle = plain;
	assert_eq(xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph), 0);
	close(ph.fd);

	/* The USSE heaps take code objects only, and code goes nowhere else. */
	assert_eq(vm_map(fd, vm, plain, 0x37000000, 0, PAGE), -EINVAL);
	assert_eq(vm_map(fd, vm, plain, 0x3c000000, 0, PAGE), -EINVAL);
	assert_eq(vm_map(fd, vm, code, 0x3e000000, 0, PAGE), -EINVAL);
	assert_eq(vm_map(fd, vm, code, 0x2e000000, 0, PAGE), -EINVAL);	/* PDS pixel */
	assert_eq(vm_map(fd, vm, code, 0x37000000, 0, 2 * PAGE), 0);
	assert_eq(vm_map(fd, vm, code, 0x3c000000, 0, PAGE), 0);
}

/*
 * PRIME: an object's dma-buf maps on the CPU only with ALLOW_CPU_ACCESS, and an
 * import goes where any data object goes (powervr_sgx_prime: a KMS device's).
 */
static void t_prime(int fd)
{
	struct drm_prime_handle ph = { .flags = DRM_CLOEXEC | DRM_RDWR }, im = {};
	struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args mo = {};
	uint32_t plain = create_bo(fd, 2 * PAGE, 0);
	uint32_t cpu = create_bo(fd, PAGE, DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS);
	int fd2 = open_dev(), dfd;
	uint32_t vm2, *p, *q;

	assert_true(fd2 >= 0);
	ph.handle = plain;
	do_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph);
	assert_true(mmap(NULL, PAGE, PROT_READ, MAP_SHARED, ph.fd, 0) == MAP_FAILED);
	assert_eq(errno, EINVAL);
	close(ph.fd);

	ph.handle = cpu;
	do_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph);
	dfd = ph.fd;
	p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
	assert_true(p != MAP_FAILED);
	p[0] = 0x5a5a1234;

	/* into the same file: the handle it already has */
	im.fd = dfd;
	do_ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &im);
	assert_eq(im.handle, cpu);

	/* into another file: a data object there */
	im = (struct drm_prime_handle){ .fd = dfd };
	do_ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &im);
	vm2 = create_vm(fd2);
	assert_eq(vm_map(fd2, vm2, im.handle, 0x37000000, 0, PAGE), -EINVAL);	/* USSE: code only */
	assert_eq(vm_map(fd2, vm2, im.handle, 0x3e000000, 0, 2 * PAGE), -EINVAL);	/* past its end */
	assert_eq(vm_map(fd2, vm2, im.handle, 0x3e000000, 0, PAGE), 0);
	mo.handle = im.handle;
	do_ioctl(fd2, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &mo);
	q = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd2, mo.offset);
	assert_true(q != MAP_FAILED);
	assert_eq(q[0], 0x5a5a1234);
	munmap(q, PAGE);
	munmap(p, PAGE);
	close(dfd);
	close(fd2);
}

/* Every corpus instruction passes, at the middle of a 4-page object. */
static void t_upload_code_corpus(int fd)
{
	uint32_t n = sizeof(usse_corpus) / sizeof(usse_corpus[0]);
	uint32_t bo = create_bo(fd, 4 * PAGE, DRM_POWERVR_SGX_BO_CODE);
	uint64_t w[sizeof(usse_corpus) / sizeof(usse_corpus[0])], bad;

	for (uint32_t i = 0; i < n; i++)
		w[i] = (uint64_t)usse_corpus[i].hi << 32 | usse_corpus[i].lo;
	assert_eq(upload(fd, bo, 2 * PAGE, w, sizeof(w), &bad), 0);
	assert_eq(bad, ~0ull);
	/* one at a time, so a refusal names the instruction */
	for (uint32_t i = 0; i < n; i++)
		if (upload(fd, bo, 2 * PAGE + 8 * i, &w[i], 8, NULL))
			fail("refused: %s (%08x %08x)", usse_corpus[i].text, usse_corpus[i].lo,
			     usse_corpus[i].hi);
	printf("  %u corpus instructions accepted\n", n);
}

/*
 * Words from the isaspec encoder (mesa/src/sgx/isa), each with what must
 * happen at word 2 of a 3-word upload at the start of a 1-page object.
 */
static const struct {
	uint32_t lo, hi;
	const char *what;
	int ok;
} crafted[] = {
	{ 0x2000001c, 0xfe210000, "ldr r0, #28", 0 },
	{ 0x20264000, 0xfe210081, "ldr pa1, #3200 [drc=1]", 0 },
	{ 0x00400003, 0xfe200000, "ldr r2, r3", 0 },
	{ 0x20064080, 0xfe290000, "str #3200, r1", 0 },
	{ 0x00000203, 0xfe280800, "str r3, r4 [nosched]", 0 },
	{ 0x00000180, 0xf80000c0, "setl r3", 0 },
	{ 0x00a00000, 0xf8000100, "savl r5", 0 },
	{ 0x0003fffd, 0xf8000040, "br #-3 (before the object)", 0 },
	{ 0x0003fffe, 0xf8000040, "br #-2 (its first word)", 1 },
	{ 0x000001fd, 0xf8000040, "br #509 (its last word)", 1 },
	{ 0x000001fe, 0xf8000040, "br #510 (past its end)", 0 },
	{ 0x0001ffff, 0xf8000040, "br #131071", 0 },
	{ 0x0000001c, 0xfe210008, "ldr r0, #28 with a stray bit (h3): no form", 0 },
	{ 0xffffffff, 0xffffffff, "all ones: no form", 0 },
	{ 0xffffffff, 0x00000000, "fmad with every operand bit set", 1 },
	{ 0x00000000, 0x00000000, "zero (what an object holds before an upload)", 1 },
};

static void t_upload_code_reject(int fd)
{
	uint32_t bo = create_bo(fd, PAGE, DRM_POWERVR_SGX_BO_CODE);
	uint64_t w[3] = { 0, 0, 0 }, bad;

	for (unsigned int i = 0; i < sizeof(crafted) / sizeof(crafted[0]); i++) {
		int r;

		w[2] = (uint64_t)crafted[i].hi << 32 | crafted[i].lo;
		r = upload(fd, bo, 0, w, sizeof(w), &bad);
		if (crafted[i].ok ? r != 0 : r != -EINVAL || bad != 16)
			fail("%s: %d, bad offset %#llx", crafted[i].what, r, (unsigned long long)bad);
	}
	/* a refusal names the first bad word */
	w[0] = (uint64_t)crafted[0].hi << 32 | crafted[0].lo;
	w[1] = w[2] = (uint64_t)crafted[3].hi << 32 | crafted[3].lo;
	assert_eq(upload(fd, bo, 0x100, w, sizeof(w), &bad), -EINVAL);
	assert_eq(bad, 0x100);
}

static void t_upload_code_invalid(int fd)
{
	uint32_t code = create_bo(fd, PAGE, DRM_POWERVR_SGX_BO_CODE), plain = create_bo(fd, PAGE, 0);
	uint64_t w[2] = { 0, 0 };
	struct drm_powervr_sgx_ioctl_upload_code_args a = {
		.handle = code, .size = 16, .pointer = (uintptr_t)w,
	};

	assert_eq(upload(fd, code, 0, w, 16, NULL), 0);
	assert_eq(upload(fd, plain, 0, w, 16, NULL), -EINVAL);	/* not a code object */
	assert_eq(upload(fd, 0xdead, 0, w, 16, NULL), -ENOENT);
	assert_eq(upload(fd, code, 4, w, 8, NULL), -EINVAL);	/* 8-byte aligned */
	assert_eq(upload(fd, code, 0, w, 12, NULL), -EINVAL);
	assert_eq(upload(fd, code, 0, w, 0, NULL), -EINVAL);
	assert_eq(upload(fd, code, PAGE - 8, w, 16, NULL), -EINVAL);	/* past the end */
	assert_eq(upload(fd, code, ~0ull - 7, w, 16, NULL), -EINVAL);
	assert_eq(upload(fd, code, 0, (void *)8, 16, NULL), -EFAULT);
	a.flags = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE, &a, EINVAL);
}

/* While mapped, uploads only append; once unmapped, anywhere again. */
static void t_upload_code_mapped(int fd)
{
	uint32_t vm = create_vm(fd), bo = create_bo(fd, 2 * PAGE, DRM_POWERVR_SGX_BO_CODE);
	uint64_t w[32] = { 0 };

	assert_eq(upload(fd, bo, 0, w, 0x100, NULL), 0);
	assert_eq(upload(fd, bo, 0, w, 0x100, NULL), 0);	/* not mapped: again */
	assert_eq(vm_map(fd, vm, bo, 0x37000000, 0, 2 * PAGE), 0);
	assert_eq(upload(fd, bo, 0x80, w, 0x100, NULL), -EBUSY);	/* overlaps uploaded code */
	assert_eq(upload(fd, bo, 0xf8, w, 8, NULL), -EBUSY);
	assert_eq(upload(fd, bo, 0x100, w, 0x100, NULL), 0);
	assert_eq(upload(fd, bo, 0x400, w, 0x100, NULL), 0);	/* a gap is fine */
	assert_eq(upload(fd, bo, 0x300, w, 0x100, NULL), -EBUSY);	/* behind the furthest */
	assert_eq(vm_unmap(fd, vm, 0x37000000, 2 * PAGE), 0);
	assert_eq(upload(fd, bo, 0, w, 0x100, NULL), 0);
}

/* RT datasets */

static uint32_t create_rtds(int fd, uint32_t ctx, uint32_t w, uint32_t h, int *err)
{
	struct drm_powervr_sgx_ioctl_create_rt_dataset_args a = {
		.context_handle = ctx, .width = w, .height = h, .samples = 1,
	};

	*err = xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET, &a);
	return a.handle;
}

static void t_rtds_create_destroy(int fd)
{
	uint32_t ctx = create_ctx(fd, create_vm(fd), DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	struct drm_powervr_sgx_ioctl_destroy_rt_dataset_args d = {};
	int err;

	d.handle = create_rtds(fd, ctx, 720, 480, &err);
	assert_eq(err, 0);
	assert_true(d.handle);
	create_rtds(fd, ctx, 64, 32, &err);	/* the other macrotile mode */
	assert_eq(err, 0);
	do_ioctl(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET, &d);
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET, &d, ENOENT);
	d.handle = 0;
	d._padding_4 = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET, &d, EINVAL);
}

static void t_rtds_invalid(int fd)
{
	uint32_t vm = create_vm(fd);
	uint32_t ctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t xctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
	struct drm_powervr_sgx_ioctl_create_rt_dataset_args a = {
		.context_handle = ctx, .width = 64, .height = 64, .samples = 1,
	};
	int err;

	a.samples = 4;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET, &a, EINVAL);
	a.samples = 1;
	a.flags = 1;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET, &a, EINVAL);
	a.flags = 0;
	a.width = 0;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET, &a, EINVAL);
	a.width = 8192;
	do_ioctl_err(fd, DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET, &a, EINVAL);
	create_rtds(fd, xctx, 64, 64, &err);	/* a transfer context has no PB */
	assert_eq(err, -EINVAL);
	create_rtds(fd, 0xdead, 64, 64, &err);
	assert_eq(err, -ENOENT);
}

/* Render streams: every case fails validation, so nothing reaches the GPU. */

struct rstreams {
	uint64_t render[10];
	uint64_t kick[3];
	struct drm_powervr_sgx_ta_kick k[3];
	struct drm_powervr_sgx_job j;
};

static void rstreams_init(struct rstreams *r, uint32_t ctx, uint32_t rtds, uint64_t pds, uint64_t vdm)
{
	uint32_t *r32 = (uint32_t *)r->render, *k32 = (uint32_t *)r->kick;

	memset(r, 0, sizeof(*r));
	r32[0] = 80;
	r->render[3] = pds;			/* pixel event */
	r->render[4] = pds + 0x40;		/* bg secondary */
	r->render[5] = pds + 0x80;		/* bg primary */
	r32[0x30 / 4] = 5;
	r32[0x34 / 4] = 1;
	r32[0x38 / 4] = 2;
	k32[0] = 20;
	r->kick[1] = vdm;
	r->k[0] = (struct drm_powervr_sgx_ta_kick){ .cmd_stream = (uintptr_t)r->kick, .cmd_stream_len = 20 };
	r->k[1] = r->k[0];
	r->k[2] = r->k[0];
	r->j = (struct drm_powervr_sgx_job){
		.type = DRM_POWERVR_SGX_JOB_TYPE_RENDER, .context_handle = ctx,
		.cmd_stream = (uintptr_t)r->render, .cmd_stream_len = 80,
		.ta_kicks = DRM_POWERVR_SGX_OBJ_ARRAY(1, r->k), .rt_dataset_handle = rtds,
	};
}

static void t_submit_render_invalid(int fd)
{
	uint32_t vm = create_vm(fd), ctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t ctx2 = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t xctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
	uint32_t pds = create_bo(fd, PAGE, 0), gen = create_bo(fd, PAGE, 0), rtds, rtds2;
	uint32_t *r32;
	struct rstreams r;
	int err;

	rtds = create_rtds(fd, ctx, 128, 128, &err);
	assert_eq(err, 0);
	rtds2 = create_rtds(fd, ctx2, 128, 128, &err);
	assert_eq(err, 0);
	assert_eq(vm_map(fd, vm, pds, 0x2e100000, 0, PAGE), 0);
	assert_eq(vm_map(fd, vm, gen, 0x3e100000, 0, PAGE), 0);
	r32 = (uint32_t *)r.render;

#define REJECT(err_, ...) do { rstreams_init(&r, ctx, rtds, 0x2e100000, 0x3e100000); \
		__VA_ARGS__; assert_eq(submit(fd, &r.j, 1, NULL), -(err_)); } while (0)

	REJECT(EINVAL, r32[0] = 72);			/* main length is exact */
	REJECT(EINVAL, r32[1] = 1);			/* header word must be zero */
	REJECT(EINVAL, r.j.cmd_stream_len = 88);	/* no trailing bytes */
	REJECT(EINVAL, r.j.cmd_stream_len = 8192);
	REJECT(EINVAL, r.j.cmd_stream += 4);		/* 8-byte aligned */
	REJECT(EFAULT, r.j.cmd_stream = 8);
	REJECT(EINVAL, r.j.ta_kicks.count = 0);
	REJECT(EOPNOTSUPP, r.j.ta_kicks.count = 3);	/* middle kicks: G12 */
	REJECT(EINVAL, r.j.ta_kicks.count = 2; r.k[1].flags = 1);
	REJECT(EINVAL, r.k[0].flags = 1);
	REJECT(EINVAL, ((uint32_t *)r.kick)[0] = 24);
	REJECT(EINVAL, r.kick[1] = 0x3e100002);		/* VDM base 4-byte aligned */
	REJECT(EINVAL, r.kick[1] = 0x3e200000);		/* not mapped */
	REJECT(EINVAL, ((uint32_t *)r.kick)[4] = 1);	/* ta_flags2 bits outside the mask */
	REJECT(EINVAL, r.render[3] = 0x2e200000);	/* pixel event not mapped */
	REJECT(EINVAL, r.render[3] = 0x3e100000);	/* pixel event outside PDS pixel */
	REJECT(EINVAL, r.render[4] = 0x2e100008);	/* PDS addresses 16-byte aligned */
	REJECT(EINVAL, r.render[5] = 0x100000000ull);	/* below 2^32 */
	REJECT(EINVAL, r32[0x30 / 4] = 0);		/* rows 1..63 */
	REJECT(EINVAL, r32[0x34 / 4] = 64);
	REJECT(EINVAL, r32[0x3c / 4] = 0x10000);	/* ZLS without a depth address */
	REJECT(EINVAL, r32[0x3c / 4] = 0x8);		/* ZLS bits 3-11 are the kernel's */
	REJECT(EINVAL, r32[0x40 / 4] = 0x100);		/* stencil flag without a stencil address */
	REJECT(EINVAL, r32[0x40 / 4] = 0x1);		/* 3D flags outside the mask */
	REJECT(EINVAL, r.render[1] = 0x3e100000);	/* depth not mapped for the whole target */
	REJECT(EINVAL, r.j.flags = 1);			/* render jobs have no flags yet */
	REJECT(EINVAL, r.j.rt_dataset_handle = rtds2);	/* another context's dataset */
	REJECT(ENOENT, r.j.rt_dataset_handle = 0xdead);
	REJECT(EINVAL, r.j.context_handle = xctx);	/* a render job on a transfer context */
#undef REJECT
}

/* Transfer streams: every case fails validation, so nothing reaches the GPU. */

struct xstream {
	uint64_t s[6];
	struct drm_powervr_sgx_job j;
};

static void xstream_init(struct xstream *x, uint32_t ctx, uint64_t bg, uint64_t pds)
{
	uint32_t *s32 = (uint32_t *)x->s;

	memset(x, 0, sizeof(*x));
	s32[0] = 48;
	x->s[1] = bg;
	x->s[2] = pds;			/* pixel event */
	s32[6] = 5;			/* its rows */
	s32[9] = 1;			/* last tile 1, 3 */
	s32[10] = 3;
	s32[11] = 0x3f800000;
	x->j = (struct drm_powervr_sgx_job){
		.type = DRM_POWERVR_SGX_JOB_TYPE_TRANSFER, .context_handle = ctx,
		.cmd_stream = (uintptr_t)x->s, .cmd_stream_len = 48,
	};
}

static void t_submit_transfer_invalid(int fd)
{
	uint32_t vm = create_vm(fd), xctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
	uint32_t ctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t pds = create_bo(fd, PAGE, 0), gen = create_bo(fd, PAGE, 0), rtds;
	struct xstream x;
	uint32_t *s32;
	int err;

	rtds = create_rtds(fd, ctx, 64, 64, &err);
	assert_eq(err, 0);
	assert_eq(vm_map(fd, vm, pds, 0x2e100000, 0, PAGE), 0);
	assert_eq(vm_map(fd, vm, gen, 0x3e100000, 0, PAGE), 0);
	s32 = (uint32_t *)x.s;

#define REJECT(err_, ...) do { xstream_init(&x, xctx, 0x3e100000, 0x2e100000); \
		__VA_ARGS__; assert_eq(submit(fd, &x.j, 1, NULL), -(err_)); } while (0)

	REJECT(EINVAL, s32[0] = 40);			/* length is exact */
	REJECT(EINVAL, s32[1] = 1);			/* header word must be zero */
	REJECT(EINVAL, x.j.cmd_stream_len = 0);
	REJECT(EINVAL, x.j.cmd_stream += 4);		/* 8-byte aligned */
	REJECT(EFAULT, x.j.cmd_stream = 8);
	REJECT(EINVAL, x.s[1] = 0);			/* background block required */
	REJECT(EINVAL, x.s[1] = 0x3e100008);		/* 16-byte aligned */
	REJECT(EINVAL, x.s[1] = 0x3e200000);		/* not mapped */
	REJECT(EINVAL, x.s[1] = 0x3e100f00);		/* not mapped for its 0x200 bytes */
	REJECT(EINVAL, x.s[2] = 0x3e100000);		/* pixel event outside PDS pixel */
	REJECT(EINVAL, x.s[2] = 0x2e200000);		/* pixel event not mapped */
	REJECT(EINVAL, s32[6] = 0);			/* rows 1..63 */
	REJECT(EINVAL, s32[6] = 64);
	REJECT(EINVAL, s32[7] = 2);			/* first tile after the last */
	REJECT(EINVAL, s32[10] = 0x10000);		/* tile limits */
	REJECT(EINVAL, x.j.flags = 2);			/* flags outside the mask */
	REJECT(EINVAL, x.j.rt_dataset_handle = rtds);	/* transfers take no RT dataset */
	REJECT(EINVAL, x.j.ta_kicks.count = 1);		/* or kicks */
	REJECT(EINVAL, x.j.context_handle = ctx);	/* a transfer on a render context */
#undef REJECT
}

/*
 * Every reserved bit and padding word, one at a time, on an otherwise valid
 * call: each must fail with -EINVAL (bad flags, docs/UAPI.md 2).
 */
#define BIT_REJECT(what, bit, call) do { int _e = (call); \
	if (_e != -EINVAL) fail("%s bit %u: %d, expected -EINVAL", what, (unsigned int)(bit), _e); } while (0)

static void t_reserved_bits(int fd)
{
	uint32_t vm = create_vm(fd), bo = create_bo(fd, PAGE, DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS);
	uint32_t ctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t xctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
	uint32_t pds = create_bo(fd, PAGE, 0), gen = create_bo(fd, PAGE, 0), s = syncobj(fd), rtds;
	struct drm_powervr_sgx_sync_op op;
	struct drm_powervr_sgx_bo_ref ref;
	struct drm_powervr_sgx_job j;
	struct rstreams r;
	struct xstream x;
	int err;

	rtds = create_rtds(fd, ctx, 64, 64, &err);
	assert_eq(err, 0);
	assert_eq(vm_map(fd, vm, pds, 0x2e100000, 0, PAGE), 0);
	assert_eq(vm_map(fd, vm, gen, 0x3e100000, 0, PAGE), 0);

	for (unsigned int b = 2; b < 64; b++) {	/* 0 CPU access, 1 code */
		struct drm_powervr_sgx_ioctl_create_bo_args a = { .size = PAGE, .flags = 1ull << b };

		BIT_REJECT("CREATE_BO flags", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a));
	}
	for (unsigned int b = 0; b < 32; b++) {
		struct drm_powervr_sgx_ioctl_create_bo_args cb = { .size = PAGE, ._padding_c = 1u << b };
		struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args mo = { .handle = bo, ._padding_4 = 1u << b };
		struct drm_powervr_sgx_ioctl_create_vm_context_args cv = { ._padding_4 = 1u << b };
		struct drm_powervr_sgx_ioctl_destroy_vm_context_args dv = { .handle = vm, ._padding_4 = 1u << b };
		struct drm_powervr_sgx_ioctl_vm_map_args m = {
			.vm_context_handle = vm, .device_addr = 0x3e200000, .handle = bo, .size = PAGE,
			.flags = 1u << b,
		};
		struct drm_powervr_sgx_ioctl_vm_map_args m2 = m;
		struct drm_powervr_sgx_ioctl_vm_unmap_args u = {
			.vm_context_handle = vm, .device_addr = 0x3e100000, .size = PAGE, ._padding_4 = 1u << b,
		};
		struct drm_powervr_sgx_ioctl_create_context_args cc = {
			.type = DRM_POWERVR_SGX_CTX_TYPE_RENDER, .vm_context_handle = vm, .flags = 1u << b,
		};
		struct drm_powervr_sgx_ioctl_destroy_context_args dc = { .handle = ctx, ._padding_4 = 1u << b };
		struct drm_powervr_sgx_ioctl_get_context_state_args gs = { .handle = ctx, ._padding_c = 1u << b };
		struct drm_powervr_sgx_ioctl_create_rt_dataset_args cr = {
			.context_handle = ctx, .width = 64, .height = 64, .samples = 1, .flags = 1u << b,
		};
		struct drm_powervr_sgx_ioctl_destroy_rt_dataset_args dr = { .handle = rtds, ._padding_4 = 1u << b };
		struct drm_powervr_sgx_ioctl_submit_jobs_args sj = { ._padding_14 = 1u << b };

		BIT_REJECT("CREATE_BO padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &cb));
		BIT_REJECT("GET_BO_MMAP_OFFSET padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &mo));
		BIT_REJECT("CREATE_VM_CONTEXT padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT, &cv));
		BIT_REJECT("DESTROY_VM_CONTEXT padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT, &dv));
		BIT_REJECT("VM_MAP flags", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_VM_MAP, &m));
		m2._padding_14 = 1u << b;
		m2.flags = 0;
		BIT_REJECT("VM_MAP padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_VM_MAP, &m2));
		BIT_REJECT("VM_UNMAP padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_VM_UNMAP, &u));
		BIT_REJECT("CREATE_CONTEXT flags", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &cc));
		BIT_REJECT("DESTROY_CONTEXT padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT, &dc));
		BIT_REJECT("GET_CONTEXT_STATE padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE, &gs));
		BIT_REJECT("CREATE_RT_DATASET flags", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET, &cr));
		BIT_REJECT("DESTROY_RT_DATASET padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET, &dr));
		j = null_job(ctx, NULL, 0);
		sj.jobs = (struct drm_powervr_sgx_obj_array)DRM_POWERVR_SGX_OBJ_ARRAY(1, &j);
		BIT_REJECT("SUBMIT_JOBS padding", b, xioctl(fd, DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &sj));

		/* job: padding, then flags per type */
		j._padding_4c = 1u << b;
		BIT_REJECT("job padding", b, submit(fd, &j, 1, NULL));
		j = null_job(ctx, NULL, 0);
		j.flags = 1u << b;
		BIT_REJECT("null job flags", b, submit(fd, &j, 1, NULL));
		rstreams_init(&r, ctx, rtds, 0x2e100000, 0x3e100000);
		r.j.flags = 1u << b;
		BIT_REJECT("render job flags", b, submit(fd, &r.j, 1, NULL));
		rstreams_init(&r, ctx, rtds, 0x2e100000, 0x3e100000);
		r.k[0].flags = 1u << b;
		BIT_REJECT("TA kick flags", b, submit(fd, &r.j, 1, NULL));
		if (!((1u << b) & DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_FLAGS_MASK)) {
			xstream_init(&x, xctx, 0x3e100000, 0x2e100000);
			x.j.flags = 1u << b;
			BIT_REJECT("transfer job flags", b, submit(fd, &x.j, 1, NULL));
		}

		/* sync ops: bits 4-30 are reserved; handle types 2-15 do not exist */
		if (b >= 4 && b < 31) {
			op = (struct drm_powervr_sgx_sync_op)SIGNAL(s);
			op.flags |= 1u << b;
			j = null_job(ctx, &op, 1);
			BIT_REJECT("sync op flags", b, submit(fd, &j, 1, NULL));
		}
		if (b >= 2 && b < 16) {
			op = (struct drm_powervr_sgx_sync_op)SIGNAL(s);
			op.flags |= b;
			j = null_job(ctx, &op, 1);
			BIT_REJECT("sync op handle type", b, submit(fd, &j, 1, NULL));
		}
		if (b >= 1) {
			ref = (struct drm_powervr_sgx_bo_ref){ .handle = bo, .flags = 1u << b };
			j = null_job(ctx, NULL, 0);
			j.bo_refs = (struct drm_powervr_sgx_obj_array)DRM_POWERVR_SGX_OBJ_ARRAY(1, &ref);
			BIT_REJECT("BO ref flags", b, submit(fd, &j, 1, NULL));
		}
	}

	/* Nothing above was queued, and the objects are all still there. */
	assert_eq(syncobj_wait(fd, s, 0), -EINVAL);
	j = null_job(ctx, NULL, 0);
	assert_eq(submit(fd, &j, 1, NULL), 0);
}

/*
 * Addresses outside the submitting context's VM: the same addresses mapped in
 * another VM of the same file count for nothing.
 */
static void t_submit_other_vm(int fd)
{
	uint32_t vm = create_vm(fd), vm2 = create_vm(fd);
	uint32_t ctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_RENDER);
	uint32_t xctx = create_ctx(fd, vm, DRM_POWERVR_SGX_CTX_TYPE_TRANSFER);
	uint32_t pds = create_bo(fd, PAGE, 0), gen = create_bo(fd, PAGE, 0), rtds;
	struct rstreams r;
	struct xstream x;
	int err;

	rtds = create_rtds(fd, ctx, 64, 64, &err);
	assert_eq(err, 0);
	/* Mapped in vm2 only. */
	assert_eq(vm_map(fd, vm2, pds, 0x2e100000, 0, PAGE), 0);
	assert_eq(vm_map(fd, vm2, gen, 0x3e100000, 0, PAGE), 0);
	rstreams_init(&r, ctx, rtds, 0x2e100000, 0x3e100000);
	assert_eq(submit(fd, &r.j, 1, NULL), -EINVAL);
	xstream_init(&x, xctx, 0x3e100000, 0x2e100000);
	assert_eq(submit(fd, &x.j, 1, NULL), -EINVAL);

	/* Mapped in vm, then unmapped: refused again. */
	assert_eq(vm_map(fd, vm, pds, 0x2e100000, 0, PAGE), 0);
	assert_eq(vm_map(fd, vm, gen, 0x3e100000, 0, PAGE), 0);
	assert_eq(vm_unmap(fd, vm, 0x3e100000, PAGE), 0);
	rstreams_init(&r, ctx, rtds, 0x2e100000, 0x3e100000);
	assert_eq(submit(fd, &r.j, 1, NULL), -EINVAL);
	xstream_init(&x, xctx, 0x3e100000, 0x2e100000);
	assert_eq(submit(fd, &x.j, 1, NULL), -EINVAL);

	/* Addresses in the kernel's ranges, which no client can map. */
	rstreams_init(&r, ctx, rtds, 0x2e100000, 0x10000000);	/* 3D params */
	assert_eq(submit(fd, &r.j, 1, NULL), -EINVAL);
	xstream_init(&x, xctx, 0x10000000, 0x2e100000);
	assert_eq(submit(fd, &x.j, 1, NULL), -EINVAL);
}

static const struct {
	const char *name;
	void (*fn)(int fd);
} tests[] = {
	{ "dev-query-gpu-info", t_query_gpu_info },
	{ "dev-query-short-size", t_query_short },
	{ "dev-query-limits-quirks", t_query_limits_quirks },
	{ "dev-query-heap-info", t_query_heap_info },
	{ "dev-query-invalid", t_query_invalid },
	{ "create-bo", t_create_bo },
	{ "create-bo-invalid", t_create_bo_invalid },
	{ "mmap-bo", t_mmap_bo },
	{ "mmap-bo-invalid", t_mmap_bo_invalid },
	{ "vm-create-destroy", t_vm_create_destroy },
	{ "vm-map-unmap", t_vm_map_unmap },
	{ "vm-map-overlap", t_vm_map_overlap },
	{ "vm-map-kernel-ranges", t_vm_map_kernel_ranges },
	{ "vm-map-invalid", t_vm_map_invalid },
	{ "vm-handles-per-file", t_vm_handles_per_file },
	{ "context-create-destroy", t_context_create_destroy },
	{ "context-invalid", t_context_invalid },
	{ "submit-null-job", t_null_job },
	{ "submit-null-job-timeline", t_null_job_timeline },
	{ "submit-null-job-chain", t_null_job_chain },
	{ "submit-null-job-many", t_null_job_many },
	{ "submit-null-job-bo-ref", t_null_job_bo_ref },
	{ "submit-null-job-vm-destroyed", t_null_job_vm_destroyed },
	{ "submit-invalid", t_submit_invalid },
	{ "context-state-invalid", t_context_state_invalid },
	{ "submit-other-file", t_submit_other_file },
	{ "submit-wait-unsignalled", t_submit_wait_unsignalled },
	{ "rt-dataset-create-destroy", t_rtds_create_destroy },
	{ "rt-dataset-invalid", t_rtds_invalid },
	{ "submit-render-invalid", t_submit_render_invalid },
	{ "submit-transfer-invalid", t_submit_transfer_invalid },
	{ "reserved-bits", t_reserved_bits },
	{ "code-bo", t_code_bo },
	{ "prime", t_prime },
	{ "upload-code-corpus", t_upload_code_corpus },
	{ "upload-code-reject", t_upload_code_reject },
	{ "upload-code-invalid", t_upload_code_invalid },
	{ "upload-code-mapped", t_upload_code_mapped },
	{ "submit-other-vm", t_submit_other_vm },
};

int main(int argc, char **argv)
{
	int failures = 0, run = 0;

	if (argc > 1 && !strcmp(argv[1], "-l")) {
		for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
			puts(tests[i].name);
		return 0;
	}
	for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int match = argc < 2, status, fd;
		pid_t pid;

		for (int a = 1; a < argc; a++)
			match |= !fnmatch(argv[a], tests[i].name, 0);
		if (!match)
			continue;
		cur = tests[i].name;
		fd = open_dev();
		if (fd < 0) {
			fprintf(stderr, "no powervr_sgx render node\n");
			return 77;
		}
		printf("%s\n", cur);
		fflush(stdout);
		pid = fork();
		if (!pid) {
			alarm(30);
			tests[i].fn(fd);
			exit(0);
		}
		waitpid(pid, &status, 0);
		close(fd);
		run++;
		if (WIFEXITED(status) && !WEXITSTATUS(status)) {
			printf("  ok\n");
		} else {
			printf("  FAILED (%s %d)\n", WIFSIGNALED(status) ? "signal" : "exit",
			       WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
			failures++;
		}
	}
	printf("%d/%d passed\n", run - failures, run);
	return failures;
}
