/*
 * uspsub: run the DDK's GLES driver on USP binaries from another compiler.
 *
 * LD_PRELOAD it in front of a GLES program. libglslcompiler calls libusc's
 * PVRUniFlexCompileToUspBin once per shader; this calls the real one (so the
 * driver's own bookkeeping is unchanged), saves its result to
 * $USPSUB_SAVE/<tag>.usp, and, when $USPSUB_DIR/<tag>.usp exists, hands that
 * binary to the driver instead. <tag> is whatever the program last passed to
 * uspsub_tag(). The substitute is written into libusc's own allocation
 * (realloc'd if it's bigger), so it is freed the usual way.
 *
 *   zig cc -target x86-linux-gnu.2.17 -O2 -shared -fPIC -o uspsub.so uspsub.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int (*compile_fn)(void *, void *, void *, void *, void *, void **);

static char tag[128] = "untagged";

void uspsub_tag(const char *t)
{
	snprintf(tag, sizeof tag, "%s", t);
}

static void *sym(const char *name)
{
	void *f = dlsym(RTLD_NEXT, name);
	if (!f) {
		/* libusc came in through libGLESv2's local dlopen */
		void *h = dlopen("libusc.so", RTLD_NOW | RTLD_NOLOAD);
		f = h ? dlsym(h, name) : NULL;
	}
	return f;
}

int PVRUniFlexCompileToUspBin(void *ctx, void *flags, void *prog, void *params, void *x, void **out)
{
	static compile_fn real;
	const char *save = getenv("USPSUB_SAVE"), *dir = getenv("USPSUB_DIR");
	char path[512];
	int fd, r;

	if (!real && !(real = (compile_fn)sym("PVRUniFlexCompileToUspBin")))
		return -1;
	r = real(ctx, flags, prog, params, x, out);
	if (r || !out || !*out)
		return r;
	if (save) {
		uint32_t size = ((uint32_t *)*out)[2] + 12;
		snprintf(path, sizeof path, "%s/%s.usp", save, tag);
		if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
			if (write(fd, *out, size) != (ssize_t)size)
				perror(path);
			close(fd);
		}
	}
	if (dir) {
		struct stat st;
		snprintf(path, sizeof path, "%s/%s.usp", dir, tag);
		if (stat(path, &st) == 0 && (fd = open(path, O_RDONLY)) >= 0) {
			uint32_t have = ((uint32_t *)*out)[2] + 12;
			void *dst = *out;
			/* write it into libusc's own allocation (the caller frees that as
			 * usual); grow it with realloc when it doesn't fit */
			if ((uint32_t)st.st_size > have)
				dst = realloc(*out, st.st_size);
			if (dst && read(fd, dst, st.st_size) == st.st_size) {
				*out = dst;
				fprintf(stderr, "uspsub: %s <- %s (%ld bytes, libusc's %u)\n", tag, path,
					(long)st.st_size, have);
			}
			close(fd);
		}
	}
	return r;
}
