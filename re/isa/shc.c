/*
 * shc: compile a list of GLSL ES shaders with the DDK, nothing drawn.
 *
 *   shc <file>
 *
 * The file holds shaders separated by header lines "@frag <name>" or
 * "@vert <name>". Run under uschook.so to collect each compiled USP binary,
 * tagged with <name>. Only a context is created: no GPU work is submitted.
 *
 *   zig cc -target x86-linux-gnu.2.17 -O1 -o shc shc.c libEGL.so libGLESv2.so -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
EGLDisplay eglGetDisplay(void *);
unsigned eglInitialize(EGLDisplay, EGLint *, EGLint *);
unsigned eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
unsigned eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
unsigned eglTerminate(EGLDisplay);
unsigned glCreateShader(unsigned);
void glShaderSource(unsigned, int, const char *const *, const int *);
void glCompileShader(unsigned);
void glGetShaderiv(unsigned, unsigned, int *);
void glGetShaderInfoLog(unsigned, int, int *, char *);
void glDeleteShader(unsigned);

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81

static void compile(void (*tag)(const char *), unsigned type, const char *name, const char *src)
{
	unsigned s = glCreateShader(type);
	int ok = 0;
	char log[512] = "";

	if (tag)
		tag(name);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
		glGetShaderInfoLog(s, sizeof log, NULL, log);
	printf("%s %s %s\n", ok ? "ok  " : "FAIL", name, log);
	glDeleteShader(s);
}

int main(int argc, char **argv)
{
	const EGLint cfg_attr[] = { 0x3033, 0x0001 /* EGL_PBUFFER_BIT */, 0x3040, 0x0004, 0x3038 };
	const EGLint pb_attr[] = { 0x3057, 16, 0x3056, 16, 0x3038 };
	const EGLint ctx_attr[] = { 0x3098, 2, 0x3038 };
	void (*tag)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "uschook_tag");
	static char text[1 << 20];
	char *p, *next, name[64] = "";
	unsigned type = 0;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLint n, maj, min;
	size_t len;
	FILE *f;

	if (argc < 2 || !(f = fopen(argv[1], "r")))
		return fprintf(stderr, "usage: shc <file>\n"), 2;
	len = fread(text, 1, sizeof text - 1, f);
	text[len] = 0;
	fclose(f);

	dpy = eglGetDisplay(NULL);
	if (!eglInitialize(dpy, &maj, &min) || !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1)
		return fprintf(stderr, "shc: egl init failed\n"), 1;
	surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
	if (!eglMakeCurrent(dpy, surf, surf, eglCreateContext(dpy, cfg, NULL, ctx_attr)))
		return fprintf(stderr, "shc: no context\n"), 1;

	for (p = text; p && *p; p = next) {
		next = strstr(p, "\n@");
		if (next)
			*next++ = 0;
		if (*p == '@') {
			char kind[8];
			char *body = strchr(p, '\n');

			if (sscanf(p, "@%7s %63s", kind, name) != 2 || !body)
				continue;
			type = !strcmp(kind, "vert") ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
			compile(tag, type, name, body + 1);
		}
	}
	eglTerminate(dpy);
	return 0;
}
