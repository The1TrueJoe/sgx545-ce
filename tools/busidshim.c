/*
 * busidshim — make Cedarview's DDK 1.7 userspace find the EA-3's SGX.
 *
 * libsrv_um.so calls drmOpen(NULL, "pci:0000:00:02.0"), with that bus id
 * hardcoded as a string constant. That is where the SGX sits on Cedarview:
 * bus 0, device 2. On the CE5300 it is its own PCI function one bus further
 * out, 0000:01:02.0, so drmOpenByBusid() matches nothing and returns -1 --
 * which is why SrvInit() failed with INIT_FAILURE and the kernel logged
 * nothing at all: userspace never got as far as an ioctl.
 *
 * The blob cannot be modified (Intel's licence forbids it, and patching a
 * string in a signed-nothing binary is fragile anyway), so intercept the call
 * instead and correct the bus id on the way through. Everything else is passed
 * to the real libdrm untouched.
 *
 *   i686-linux-gnu-gcc -shared -fPIC -o busidshim.so busidshim.c -ldl
 *   LD_PRELOAD=./busidshim.so ./sgxinit
 *
 * SGX_BUSID overrides the replacement, so this is not pinned to one board.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* Where Cedarview's SGX lives, and what its userspace therefore asks for. */
#define CEDARVIEW_BUSID "pci:0000:00:02.0"
/* Where the CE5300's SGX actually lives. */
#define CE5300_BUSID    "pci:0000:01:02.0"

int drmOpen(const char *name, const char *busid)
{
	static int (*real_drmOpen)(const char *, const char *);
	const char *want;

	if (!real_drmOpen) {
		real_drmOpen = dlsym(RTLD_NEXT, "drmOpen");
		if (!real_drmOpen) {
			fprintf(stderr, "busidshim: no real drmOpen: %s\n", dlerror());
			return -1;
		}
	}

	if (busid && strcmp(busid, CEDARVIEW_BUSID) == 0) {
		want = getenv("SGX_BUSID");
		if (!want)
			want = CE5300_BUSID;
		fprintf(stderr, "busidshim: drmOpen busid %s -> %s\n", busid, want);
		busid = want;
	}

	return real_drmOpen(name, busid);
}
