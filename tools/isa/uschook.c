/*
 * uschook: save every USP binary the DDK's shader compiler produces.
 *
 * LD_PRELOAD it in front of a GLES program. libglslcompiler calls libusc's
 * PVRUniFlexCompileToUspBin through its PLT, so this definition wins; each
 * result is written to $USCHOOK_DIR/<n>-<tag>.usp (default /tmp/usc), where
 * <tag> is whatever the program last passed to uschook_tag(). The blob's size
 * isn't known here, so up to 64 KB is written in small pieces, stopping where
 * memory stops being readable (write(2) fails with EFAULT rather than faulting).
 *
 *   zig cc -target x86-linux-gnu.2.17 -O2 -shared -fPIC -o uschook.so uschook.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*compile_fn)(void *, void *, void *, void *, void *, void **);

static char tag[64] = "untagged";
static int n;

void uschook_tag(const char *t)
{
	snprintf(tag, sizeof tag, "%s", t);
}

int PVRUniFlexCompileToUspBin(void *ctx, void *flags, void *prog, void *params, void *x, void **out)
{
	static compile_fn real;
	const char *dir = getenv("USCHOOK_DIR");
	char path[256];
	int fd, r;

	if (!real)
		real = (compile_fn)dlsym(RTLD_NEXT, "PVRUniFlexCompileToUspBin");
	if (!real) {
		/* libusc came in through libGLESv2's local dlopen: not in RTLD_NEXT's scope */
		void *h = dlopen("libusc.so", RTLD_NOW | RTLD_NOLOAD);

		real = h ? (compile_fn)dlsym(h, "PVRUniFlexCompileToUspBin") : NULL;
		if (!real)
			return -1;
	}
	r = real(ctx, flags, prog, params, x, out);
	if (r || !out || !*out)
		return r;
	snprintf(path, sizeof path, "%s/%03d-%s.usp", dir ? dir : "/tmp/usc", n++, tag);
	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		for (size_t o = 0; o < 65536; o += 256)
			if (write(fd, (char *)*out + o, 256) != 256)
				break;
		close(fd);
	}
	return r;
}
