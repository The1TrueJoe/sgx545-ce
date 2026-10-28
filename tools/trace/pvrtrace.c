/*
 * pvrtrace: record what a PowerVR services client asks of the kernel.
 *
 * LD_PRELOAD it in front of any DDK program. It writes one binary log
 * ($PVRTRACE_OUT, default /tmp/pvrtrace.bin) holding:
 *   - every PVR services bridge call on the DRM node: id, IN bytes, OUT bytes;
 *   - every mmap/munmap of that node (device memory mapped to the CPU);
 *   - before each SGX kick and before each unmap, a copy of every live
 *     mapping, so the decoder sees exactly what the GPU was handed;
 *   - the same copy before SGX_DEVINITPART2 and INITSRV_DISCONNECT, so a
 *     SrvInit trace holds everything the init process wrote for the microkernel.
 * PVRTRACE_SNAPKICKS=n stops the copies after the n-th kick (long clients).
 *
 * The tracer stays dumb on purpose: no struct knowledge lives here, only the
 * outer bridge package. tools/decode/ does the interpretation, so a wrong guess
 * about a layout costs a re-decode, not a re-capture on the box.
 *
 * Builds for i386 (the Cedarview DDK) and x86_64 (our own client) alike:
 *   tools/trace/build.sh   (adds the kernel headers and this build's defines)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The bridge structs, from the GPL kernel headers (build with libpvr/abi/flags.sh). */
#include "services.h"
#include "pvr_bridge.h"
#include "sgx_bridge.h"

/* The outer package, in the caller's own ABI (pointers are native width). */
struct pvr_bridge_package {
	uint32_t id;
	uint32_t size;
	void *in;
	uint32_t in_size;
	void *out;
	uint32_t out_size;
	void *services;
};

enum {
	REC_BRIDGE = 1,	/* u32 id, i32 ret, i32 err, u32 in_n, u32 out_n, u64 ns, u64 services, in, out */
	REC_MMAP = 2,	/* u64 addr, u64 len, u32 prot, u32 flags, u64 off, u64 ret */
	REC_MUNMAP = 3,	/* u64 addr, u64 len */
	REC_SNAP = 4,	/* u32 reason, u32 kickno, u64 addr, u64 len, data */
	REC_NOTE = 5,	/* text */
	REC_UBUF = 6,	/* u32 bridge nr, u32 phase (0 before, 1 after), u64 ptr, data:
			   user memory a bridge's handler copies through a pointer in IN */
};

#define SNAP_KICK 1
#define SNAP_UNMAP 2
#define SNAP_EXIT 3
#define SNAP_INIT 4

#define MAX_MAPS 512

static struct { uintptr_t addr; size_t len; uint64_t off; } maps[MAX_MAPS];
static int nmaps;
static int log_fd = -1;
static unsigned kicks;
static unsigned long max_snap = 64u << 20;	/* skip mappings bigger than this */
static int kick_nr = -1;			/* bridge index of SGX_DOKICK */
static int snap_on_kick = 1;
static unsigned snap_kicks;			/* PVRTRACE_SNAPKICKS: copies only up to this kick */
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static int (*real_ioctl)(int, unsigned long, void *);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static void *(*real_mmap64)(void *, size_t, int, int, int, off64_t);
static int (*real_munmap)(void *, size_t);
static long (*real_syscall)(long, ...);

static void forget_map(void *a, size_t len);

static void out(const void *p, size_t n)
{
	const char *c = p;
	while (n) {
		ssize_t w = write(log_fd, c, n);
		if (w <= 0) {
			if (w < 0 && errno == EINTR)
				continue;
			return;
		}
		c += w;
		n -= (size_t)w;
	}
}

static void rec(uint32_t type, uint32_t len)
{
	uint32_t h[2] = { type, len };
	out(h, sizeof h);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void note(const char *s)
{
	rec(REC_NOTE, (uint32_t)strlen(s));
	out(s, strlen(s));
}

__attribute__((constructor)) static void init(void)
{
	const char *path = getenv("PVRTRACE_OUT");
	const char *e;
	char buf[128];

	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	real_mmap = dlsym(RTLD_NEXT, "mmap");
	real_mmap64 = dlsym(RTLD_NEXT, "mmap64");
	real_munmap = dlsym(RTLD_NEXT, "munmap");
	real_syscall = dlsym(RTLD_NEXT, "syscall");
	if ((e = getenv("PVRTRACE_MAXSNAP")))
		max_snap = strtoul(e, NULL, 0);
	if ((e = getenv("PVRTRACE_KICK")))
		kick_nr = (int)strtol(e, NULL, 0);
	else
		kick_nr = 83 + 3;	/* PVRSRV_BRIDGE_SGX_DOKICK in this build */
	if ((e = getenv("PVRTRACE_NOSNAP")))
		snap_on_kick = 0;
	if ((e = getenv("PVRTRACE_SNAPKICKS")))
		snap_kicks = strtoul(e, NULL, 0);

	log_fd = open(path ? path : "/tmp/pvrtrace.bin",
		      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (log_fd < 0)
		return;
	out("PVRT", 4);
	{
		uint32_t v[2] = { 2, (uint32_t)sizeof(void *) };
		out(v, sizeof v);
	}
	snprintf(buf, sizeof buf, "pid %d ptr %zu", getpid(), sizeof(void *));
	note(buf);
}

static int is_drm_fd(int fd)
{
	static int cached = -2;
	char p[64], l[64];
	ssize_t n;

	if (fd == cached)
		return 1;
	snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
	n = readlink(p, l, sizeof l - 1);
	if (n <= 0)
		return 0;
	l[n] = 0;
	if (strncmp(l, "/dev/dri/", 9))
		return 0;
	cached = fd;
	return 1;
}

static void snap_one(uint32_t why, uintptr_t a, size_t len)
{
	uint64_t h[2] = { a, len };
	uint32_t w[2] = { why, kicks };

	if (len > max_snap)
		return;
	rec(REC_SNAP, (uint32_t)(sizeof w + sizeof h + len));
	out(w, sizeof w);
	out(h, sizeof h);
	out((void *)a, len);
}

static void snap_all(uint32_t why)
{
	if (snap_kicks && kicks > snap_kicks && why != SNAP_INIT)
		return;
	for (int i = 0; i < nmaps; i++)
		snap_one(why, maps[i].addr, maps[i].len);
}

static void ubuf(uint32_t nr, uint32_t phase, const void *p, size_t len)
{
	uint32_t h[2] = { nr, phase };
	uint64_t a = (uintptr_t)p;

	if (!p || !len || len > (16u << 20))
		return;
	rec(REC_UBUF, (uint32_t)(sizeof h + sizeof a + len));
	out(h, sizeof h);
	out(&a, sizeof a);
	out(p, len);
}

#define NR(x) ((x) & 0xff)

/* Save what the kernel will read (phase 0) or wrote (phase 1) through IN's pointers. */
static void follow(const struct pvr_bridge_package *pkg, uint32_t phase)
{
	uint32_t nr = pkg->id & 0xff;

	if (!pkg->in)
		return;
	if (nr == NR(PVRSRV_BRIDGE_SGX_REGISTER_HW_RENDER_CONTEXT) && !phase) {
		PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_RENDER_CONTEXT *in = pkg->in;
		ubuf(nr, phase, in->pHWRenderContextCpuVAddr, in->ui32HWRenderContextSize);
	} else if (nr == NR(PVRSRV_BRIDGE_SGX_REGISTER_HW_TRANSFER_CONTEXT) && !phase) {
		PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_TRANSFER_CONTEXT *in = pkg->in;
		ubuf(nr, phase, in->pHWTransferContextCpuVAddr, in->ui32HWTransferContextSize);
	} else if (nr == NR(PVRSRV_BRIDGE_SGX_GETMISCINFO)) {
		PVRSRV_BRIDGE_IN_SGXGETMISCINFO *in = pkg->in;
		ubuf(nr, phase, in->psMiscInfo, sizeof(SGX_MISC_INFO));
	} else if (nr == NR(PVRSRV_BRIDGE_SGX_DOKICK) && !phase) {
		PVRSRV_BRIDGE_IN_DOKICK *in = pkg->in;
		ubuf(nr, phase, in->sCCBKick.pahDstSyncHandles,
		     in->sCCBKick.ui32NumDstSyncObjects * sizeof(IMG_HANDLE));
	} else if (nr == NR(PVRSRV_BRIDGE_WRAP_EXT_MEMORY) && !phase) {
		PVRSRV_BRIDGE_IN_WRAP_EXT_MEMORY *in = pkg->in;
		ubuf(nr, phase, in->psSysPAddr, in->ui32NumPageTableEntries * sizeof(IMG_SYS_PHYADDR));
	}
}

/* glibc declares ioctl(int, unsigned long, ...); the request is all we need. */
int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	struct pvr_bridge_package *pkg;
	int ret, err;
	uint64_t t;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (log_fd < 0 || ((req >> 8) & 0xff) != 'd' || ((req >> 16) & 0x3fff) != sizeof *pkg ||
	    !is_drm_fd(fd))
		return real_ioctl(fd, req, arg);

	pkg = arg;
	pthread_mutex_lock(&lock);
	if (snap_on_kick && (int)(pkg->id & 0xff) == kick_nr) {
		kicks++;
		snap_all(SNAP_KICK);
	}
	if ((pkg->id & 0xff) == NR(PVRSRV_BRIDGE_SGX_DEVINITPART2) ||
	    (pkg->id & 0xff) == NR(PVRSRV_BRIDGE_INITSRV_DISCONNECT))
		snap_all(SNAP_INIT);
	follow(pkg, 0);
	t = now_ns();
	ret = real_ioctl(fd, req, arg);
	err = ret < 0 ? errno : 0;
	follow(pkg, 1);
	{
		uint32_t h[5] = { pkg->id, (uint32_t)ret, (uint32_t)err, pkg->in_size, pkg->out_size };
		uint32_t ins = pkg->in ? pkg->in_size : 0, outs = pkg->out ? pkg->out_size : 0;

		h[3] = ins;
		h[4] = outs;
		uint64_t svc = (uintptr_t)pkg->services;

		rec(REC_BRIDGE, (uint32_t)(sizeof h + 16 + ins + outs));
		out(h, sizeof h);
		out(&t, 8);
		out(&svc, 8);
		if (ins)
			out(pkg->in, ins);
		if (outs)
			out(pkg->out, outs);
	}
	pthread_mutex_unlock(&lock);
	errno = err;
	return ret;
}

static void note_map(void *r, size_t len, int prot, int flags, int fd, uint64_t off)
{
	uint64_t h[5] = { (uintptr_t)r, len, ((uint64_t)(uint32_t)flags << 32) | (uint32_t)prot, off,
			  (uintptr_t)r };

	if (log_fd < 0 || r == MAP_FAILED || !is_drm_fd(fd))
		return;
	pthread_mutex_lock(&lock);
	rec(REC_MMAP, sizeof h);
	out(h, sizeof h);
	if (nmaps < MAX_MAPS) {
		maps[nmaps].addr = (uintptr_t)r;
		maps[nmaps].len = len;
		maps[nmaps].off = off;
		nmaps++;
	}
	pthread_mutex_unlock(&lock);
}

void *mmap(void *a, size_t len, int prot, int flags, int fd, off_t off)
{
	void *r;

	if (!real_mmap)
		real_mmap = dlsym(RTLD_NEXT, "mmap");
	r = real_mmap(a, len, prot, flags, fd, off);
	if (fd >= 0)
		note_map(r, len, prot, flags, fd, (uint64_t)(unsigned long)off);
	return r;
}

void *mmap64(void *a, size_t len, int prot, int flags, int fd, off64_t off)
{
	void *r;

	if (!real_mmap64)
		real_mmap64 = dlsym(RTLD_NEXT, "mmap64");
	r = real_mmap64(a, len, prot, flags, fd, off);
	if (fd >= 0)
		note_map(r, len, prot, flags, fd, (uint64_t)off);
	return r;
}

int munmap(void *a, size_t len)
{
	if (!real_munmap)
		real_munmap = dlsym(RTLD_NEXT, "munmap");
	forget_map(a, len);
	return real_munmap(a, len);
}

static void forget_map(void *a, size_t len)
{
	if (log_fd < 0)
		return;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < nmaps; i++) {
		if (maps[i].addr != (uintptr_t)a)
			continue;
		snap_one(SNAP_UNMAP, maps[i].addr, maps[i].len);
		maps[i] = maps[--nmaps];
		{
			uint64_t h[2] = { (uintptr_t)a, len };
			rec(REC_MUNMAP, sizeof h);
			out(h, sizeof h);
		}
		break;
	}
	pthread_mutex_unlock(&lock);
}

/*
 * libsrv_um maps device memory with a raw syscall(__NR_mmap2), so it can pass
 * the services mmap cookie as a page offset; catch that (and munmap) here.
 */
long syscall(long nr, ...)
{
	va_list ap;
	long a[6], r;

	va_start(ap, nr);
	for (int i = 0; i < 6; i++)
		a[i] = va_arg(ap, long);
	va_end(ap);
	if (!real_syscall)
		real_syscall = dlsym(RTLD_NEXT, "syscall");
	if (nr == SYS_munmap)
		forget_map((void *)a[0], (size_t)a[1]);
	r = real_syscall(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
#ifdef SYS_mmap2
	if (nr == SYS_mmap2 && a[4] >= 0)
		note_map(r == -1 ? MAP_FAILED : (void *)r, (size_t)a[1], (int)a[2], (int)a[3], (int)a[4],
			 (uint64_t)(unsigned long)a[5] << 12);
#else
	if (nr == SYS_mmap && a[4] >= 0)
		note_map(r == -1 ? MAP_FAILED : (void *)r, (size_t)a[1], (int)a[2], (int)a[3], (int)a[4],
			 (uint64_t)(unsigned long)a[5]);
#endif
	return r;
}

/* Lets a test program mark points in the log: dlsym(RTLD_DEFAULT, "pvrtrace_note"). */
void pvrtrace_note(const char *s)
{
	if (log_fd < 0)
		return;
	pthread_mutex_lock(&lock);
	note(s);
	pthread_mutex_unlock(&lock);
}

void pvrtrace_snapshot(void)
{
	if (log_fd < 0)
		return;
	pthread_mutex_lock(&lock);
	snap_all(SNAP_EXIT);
	pthread_mutex_unlock(&lock);
}
