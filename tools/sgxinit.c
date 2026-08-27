/*
 * sgxinit — do what pvrsrvctl --start does, and nothing else.
 *
 * Control4's pvrsrvctl is DDK 1.12 and links 1.12's libsrv_um/libsrv_init, so
 * it cannot be pointed at a 1.7 userspace. But the thing it actually does is
 * one call: SrvInit(), no arguments, PVRSRV_ERROR in eax, zero for success.
 * (Established by disassembling it: nothing is pushed before the call, and a
 * non-zero return is handed straight to PVRSRVGetErrorString.)
 *
 * dlopen rather than link, so an unresolved symbol is a message we can read
 * instead of the loader refusing to start the process.
 */
#include <stdio.h>
#include <dlfcn.h>

int main(void)
{
	void *um, *init;
	int (*SrvInit)(void);
	const char *(*GetErr)(int);
	int e;

	/* libsrv_um first: libsrv_init depends on its symbols, and RTLD_GLOBAL
	 * is what makes them visible to it. */
	um = dlopen("libsrv_um.so", RTLD_NOW | RTLD_GLOBAL);
	if (!um) { fprintf(stderr, "libsrv_um: %s\n", dlerror()); return 2; }
	printf("libsrv_um.so   loaded\n");

	init = dlopen("libsrv_init.so", RTLD_NOW | RTLD_GLOBAL);
	if (!init) { fprintf(stderr, "libsrv_init: %s\n", dlerror()); return 2; }
	printf("libsrv_init.so loaded\n");

	SrvInit = dlsym(init, "SrvInit");
	if (!SrvInit) { fprintf(stderr, "SrvInit: %s\n", dlerror()); return 2; }
	GetErr = dlsym(um, "PVRSRVGetErrorString");

	printf("calling SrvInit()...\n");
	fflush(stdout);

	e = SrvInit();

	if (e == 0) {
		printf("\nSrvInit() = 0  -- OK, microkernel accepted\n");
		return 0;
	}
	printf("\nSrvInit() = %d  (%s)\n", e,
	       GetErr ? GetErr(e) : "no PVRSRVGetErrorString");
	return 1;
}
