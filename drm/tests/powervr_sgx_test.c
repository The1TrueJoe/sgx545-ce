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
	assert_eq(i.core_revision & 0xffffff, 0x1000e);	/* 1.0.14 on the CE5300 */
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
	assert_eq(vm_map(fd, vm, bo, 0x37000000, 0, PAGE), 0);	/* USSE pixel */
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
	j[1].type = DRM_POWERVR_SGX_JOB_TYPE_TRANSFER;
	assert_eq(submit(fd, j, 2, &failed), -EOPNOTSUPP);
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
