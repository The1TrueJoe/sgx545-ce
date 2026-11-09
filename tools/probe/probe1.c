/*
 * probe1: one GLES 1.1 (fixed-function) frame through the DDK, for state captures.
 * A clear and one triangle with per-vertex colours; every knob is one GL call.
 *   PROBE_CLEAR="r g b a"      clear colour                         (0 0 0.5 1)
 *   PROBE_VERTS="x0 y0 ..."    triangle vertices, clip space         (-0.5 -0.5 0.5 -0.5 0 0.5)
 *   PROBE_VCOLOR="r g b a ..." colour per vertex                     (red a=0, green a=0.5, blue a=1)
 *   PROBE_PRIM=n               GL primitive enum                     (4 = GL_TRIANGLES)
 *   PROBE_ALPHATEST="func ref" glEnable(ALPHA_TEST), glAlphaFunc (func as a GL enum)
 *   PROBE_BLEND="src dst"      glEnable(BLEND), glBlendFunc
 *   PROBE_TEX=1                an 8x8 RGBA8888 checker texture, GL_MODULATE, alpha 0 / 255
 *                              on alternate texels; texcoords = 2 * the vertex position
 *   PROBE_POINTSIZE=s          glPointSize
 *   PROBE_LINEWIDTH=w          glLineWidth
 *   PROBE_SPRITE=1             glEnable(POINT_SPRITE_OES), COORD_REPLACE on unit 0
 *   PROBE_OUT=path             raw RGBA readback of the whole surface
 * Marks each phase in the pvrtrace log through pvrtrace_note when the tracer is loaded.
 *   zig cc -target x86-linux-gnu.2.17 -O1 -o probe1 probe1.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat, GLclampf;
typedef unsigned char GLubyte, GLboolean;

#define FNS(X) \
	X(EGL, EGLDisplay, eglGetDisplay, (void *)) \
	X(EGL, EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *)) \
	X(EGL, EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)) \
	X(EGL, EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, uintptr_t, const EGLint *)) \
	X(EGL, EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
	X(EGL, EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
	X(EGL, EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface)) \
	X(EGL, EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *)) \
	X(EGL, EGLBoolean, eglTerminate, (EGLDisplay)) \
	X(GL, void, glClearColor, (GLclampf, GLclampf, GLclampf, GLclampf)) \
	X(GL, void, glClear, (GLbitfield)) \
	X(GL, void, glEnable, (GLenum)) \
	X(GL, void, glDisable, (GLenum)) \
	X(GL, void, glAlphaFunc, (GLenum, GLclampf)) \
	X(GL, void, glBlendFunc, (GLenum, GLenum)) \
	X(GL, void, glPointSize, (GLfloat)) \
	X(GL, void, glLineWidth, (GLfloat)) \
	X(GL, void, glTexEnvi, (GLenum, GLenum, GLint)) \
	X(GL, void, glEnableClientState, (GLenum)) \
	X(GL, void, glVertexPointer, (GLint, GLenum, GLsizei, const void *)) \
	X(GL, void, glColorPointer, (GLint, GLenum, GLsizei, const void *)) \
	X(GL, void, glTexCoordPointer, (GLint, GLenum, GLsizei, const void *)) \
	X(GL, void, glGenTextures, (GLsizei, GLuint *)) \
	X(GL, void, glBindTexture, (GLenum, GLuint)) \
	X(GL, void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
	X(GL, void, glTexParameteri, (GLenum, GLenum, GLint)) \
	X(GL, void, glDrawArrays, (GLenum, GLint, GLsizei)) \
	X(GL, void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
	X(GL, void, glFinish, (void)) \
	X(GL, GLenum, glGetError, (void)) \
	X(GL, void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))

#define DECL(lib, ret, name, args) static ret(*name) args;
FNS(DECL)

#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES_BIT 0x0001
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_ALPHA_TEST 0x0BC0
#define GL_BLEND 0x0BE2
#define GL_TEXTURE_2D 0x0DE1
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VERTEX_ARRAY 0x8074
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_MODULATE 0x2100
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_POINT_SPRITE_OES 0x8861
#define GL_COORD_REPLACE_OES 0x8862

static void (*mark)(const char *);
static void (*snapshot)(void);

static void note(const char *s)
{
	fprintf(stderr, "probe1: %s\n", s);
	if (mark)
		mark(s);
}

static int floats(const char *env, float *v, int max, const char *def)
{
	const char *s = getenv(env);
	int n = 0;
	char *e;

	if (!s)
		s = def;
	if (!s)
		return 0;
	while (n < max) {
		float x = strtof(s, &e);
		if (e == s)
			break;
		v[n++] = x;
		s = e;
	}
	return n;
}

static int load(void)
{
	void *egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	void *gl = dlopen("libGLES_CM.so", RTLD_NOW | RTLD_GLOBAL);
	void *lib;

	if (!egl || !gl) {
		fprintf(stderr, "probe1: dlopen: %s\n", dlerror());
		return 0;
	}
#define LOAD(l, ret, name, args) \
	lib = l; \
	if (!(*(void **)&name = dlsym(lib, #name))) { \
		fprintf(stderr, "probe1: no %s\n", #name); \
		return 0; \
	}
#define EGL egl
#define GL gl
	FNS(LOAD)
#undef EGL
#undef GL
	return 1;
}

int main(void)
{
	EGLint cfg_attr[] = { EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_SURFACE_TYPE,
			      EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_NONE };
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
	static float verts[2 * 64], col[4 * 64], tc[2 * 64], f[4];
	static unsigned char tex[8 * 8 * 4];
	const char *out = getenv("PROBE_OUT");
	GLenum prim = getenv("PROBE_PRIM") ? (GLenum)atoi(getenv("PROBE_PRIM")) : 4;
	EGLint maj, min, n, w, h;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLContext ctx;
	unsigned char px[4];
	int nv, nc;

	mark = (void (*)(const char *))dlsym(RTLD_DEFAULT, "pvrtrace_note");
	snapshot = (void (*)(void))dlsym(RTLD_DEFAULT, "pvrtrace_snapshot");
	if (!load())
		return 1;
	nv = floats("PROBE_VERTS", verts, 2 * 64, "-0.5 -0.5 0.5 -0.5 0 0.5") / 2;
	nc = floats("PROBE_VCOLOR", col, 4 * 64, "1 0 0 0 0 1 0 0.5 0 0 1 1") / 4;
	for (int i = nc; i < nv; i++)
		memcpy(col + 4 * i, col + 4 * (nc - 1), 16);
	for (int i = 0; i < 2 * nv; i++)
		tc[i] = 2 * verts[i];

	note("egl init");
	dpy = eglGetDisplay(NULL);
	if (!eglInitialize(dpy, &maj, &min) || !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1)
		return note("egl failed"), 1;
	surf = eglCreateWindowSurface(dpy, cfg, 0, NULL);
	ctx = eglCreateContext(dpy, cfg, NULL, ctx_attr);
	if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx))
		return note("surface/context failed"), 1;
	eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
	glViewport(0, 0, w, h);

	note("state");
	if (floats("PROBE_ALPHATEST", f, 2, "") == 2) {
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc((GLenum)f[0], f[1]);
	}
	if (floats("PROBE_BLEND", f, 2, "") == 2) {
		glEnable(GL_BLEND);
		glBlendFunc((GLenum)f[0], (GLenum)f[1]);
	}
	if (floats("PROBE_POINTSIZE", f, 1, "") == 1)
		glPointSize(f[0]);
	if (floats("PROBE_LINEWIDTH", f, 1, "") == 1)
		glLineWidth(f[0]);
	if (getenv("PROBE_TEX") && atoi(getenv("PROBE_TEX"))) {
		GLuint t;

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				unsigned char *p = tex + 4 * (8 * y + x);

				p[0] = (unsigned char)(32 * x), p[1] = (unsigned char)(32 * y), p[2] = 255;
				p[3] = (x ^ y) & 1 ? 255 : 0;
			}
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glEnable(GL_TEXTURE_2D);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, 0, tc);
	}
	if (getenv("PROBE_SPRITE") && atoi(getenv("PROBE_SPRITE"))) {
		glEnable(GL_POINT_SPRITE_OES);
		glTexEnvi(GL_POINT_SPRITE_OES, GL_COORD_REPLACE_OES, 1);
	}
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, verts);
	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4, GL_FLOAT, 0, col);

	floats("PROBE_CLEAR", f, 4, "0 0 0.5 1");
	note("clear");
	glClearColor(f[0], f[1], f[2], f[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	note("draw");
	glDrawArrays(prim, 0, nv);
	note("finish");
	glFinish();
	if (snapshot)
		snapshot();
	note("readback");
	glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("probe1: %dx%d centre rgba(%d,%d,%d,%d) glerr %#x\n", w, h, px[0], px[1], px[2], px[3],
	       glGetError());
	if (out) {
		unsigned char *img = malloc((size_t)w * h * 4);
		FILE *fp;

		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
		if ((fp = fopen(out, "wb"))) {
			fwrite(img, 4, (size_t)w * h, fp);
			fclose(fp);
		}
	}
	note("swap");
	eglSwapBuffers(dpy, surf);
	note("terminate");
	eglTerminate(dpy);
	return 0;
}
