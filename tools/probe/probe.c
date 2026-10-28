/*
 * probe: a GLES2 program whose every input comes from the environment, so two
 * captures that differ in exactly one thing can be diffed (tools/decode/diff.py).
 *
 *   PROBE_CLEAR="r g b a"      clear colour                    (0 0 0.5 1)
 *   PROBE_COLOR="r g b a"      uniform the fragment shader writes (0 1 0 1)
 *   PROBE_VERTS="x0 y0 ..."    triangle vertices, clip space, any multiple of 3
 *   PROBE_Z=z                  every vertex at this clip z (a vec3 position; default a vec2)
 *   PROBE_DRAWS=n              draw the vertex list n times      (1)
 *   PROBE_FRAMES=n             n frames, each swapped; frame k clears with red = k/n (1)
 *   PROBE_VS / PROBE_FS        shader source overrides
 *   PROBE_PRIM=n               GL primitive enum                 (4 = GL_TRIANGLES)
 *   PROBE_ATTRS="n1 n2 ..."    extra float attributes a1, a2, ... with n components each
 *   PROBE_INTERLEAVE=1         p and the extra attributes in one interleaved array
 *   PROBE_TEX=n                n 8x8 checker textures, units 0..n-1, sampler uniforms t0..
 *   PROBE_OUT=path             raw RGBA readback of the whole surface
 *
 * Render state (each one a single GL call or enable, for one-change captures):
 *   PROBE_CONFIG="r g b a d s" EGL config sizes                  (5 6 5 0 0 0)
 *   PROBE_PBUFFER="w h"        render into a pbuffer of this size instead of the window
 *   PROBE_DEPTH="func mask"    glEnable(DEPTH_TEST), glDepthFunc, glDepthMask (func as a GL enum)
 *   PROBE_CLEARDEPTH=z         glClearDepthf, and DEPTH_BUFFER_BIT in the clear
 *   PROBE_DEPTHRANGE="n f"     glDepthRangef
 *   PROBE_STENCIL="func ref mask sfail zfail zpass wmask"  glEnable(STENCIL_TEST) + func/op/mask
 *   PROBE_CLEARSTENCIL=s       glClearStencil, and STENCIL_BUFFER_BIT in the clear
 *   PROBE_BLEND="src dst [eq]" glEnable(BLEND), glBlendFunc, glBlendEquation (GL enums)
 *   PROBE_BLENDCOLOR="r g b a" glBlendColor
 *   PROBE_CMASK="r g b a"      glColorMask (0/1)
 *   PROBE_SCISSOR="x y w h"    glEnable(SCISSOR_TEST), glScissor
 *   PROBE_VIEWPORT="x y w h"   glViewport                        (the whole surface)
 *   PROBE_CULL="face front"    glEnable(CULL_FACE), glCullFace, glFrontFace (GL enums)
 *   PROBE_POLYOFF="f u"        glEnable(POLYGON_OFFSET_FILL), glPolygonOffset
 *   PROBE_DITHER=0             glDisable(DITHER)
 *   PROBE_LINEWIDTH=w          glLineWidth
 *   PROBE_NOCLEAR=1            no glClear (the draws land on the previous buffer)
 *   PROBE_SCISSOR2="x y w h"   glScissor before every draw after the first (PROBE_DRAWS >= 2)
 *   PROBE_VIEWPORT2="x y w h"  glViewport likewise
 *   PROBE_NOSCISSOR2=1         glDisable(SCISSOR_TEST) before every draw after the first
 * Textures (PROBE_TEX >= 1; all units the same):
 *   PROBE_TEXFMT=name          rgba8888 rgb565 rgba4444 rgba5551 rgb888 l8 a8 la88   (rgba8888)
 *   PROBE_TEXSIZE="w h"        (8 8); the checker repeats every 8 texels
 *   PROBE_TEXFILTER="min mag"  GL enums                          (0x2600 0x2600 = NEAREST)
 *   PROBE_TEXWRAP="s t"        GL enums                          (0x2901 = REPEAT)
 *   PROBE_MIPMAP=1             glGenerateMipmap after the upload
 *   PROBE_TEXPAT=1             texels name their own coordinates instead of the checker:
 *                              8888/888: r = x, g = y, b = level | face<<4, a = 0x80 | x>>8 | (y>>8)<<2;
 *                              16-bit formats (and la88 as l | a<<8): y*w + x; l8/a8: (y*w + x) & 0xff
 *   PROBE_TEXFILE=path         level 0 texels from a file, raw, in the GL upload format
 *   PROBE_TEXLEVELS=n          upload levels 0..n-1 one by one (each with its own level number)
 *   PROBE_TEXCUBE=1            a cube map: all six faces uploaded (face f in the pattern)
 *
 * Render to texture (the triangle pass goes into an FBO, the window samples it):
 *   PROBE_FBO="w h fmt"        colour texture rgba8888 rgb565 rgba4444 rgba5551 rgb888
 *   PROBE_FBODS="d s"          depth / stencil renderbuffers (16 0, 0 8, 24 8 = packed)
 *   PROBE_FBOCLEAR="r g b a"   the window's clear in the sampling pass   (0.2 0.2 0.2 1)
 *   PROBE_QUAD="x0 y0 x1 y1"   the window quad that samples the FBO      (-0.75 -0.75 0.75 0.75)
 *   PROBE_FBOREENTER="x0 y0 x1 y1 x2 y2 z r g b a"  after sampling once, draw this triangle
 *                              (at clip z, colour) into the FBO again without a clear,
 *                              then sample again: the FBO's depth must survive the window render
 *
 * Marks each phase in the pvrtrace log through pvrtrace_note when the tracer is loaded.
 * EGL and GLES are loaded with dlopen, so the binary has no DT_NEEDED on the DDK's
 * SONAME-less libraries:
 *   zig cc -target x86-linux-gnu.2.17 -O1 -o probe probe.c -ldl
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
typedef float GLfloat;
typedef unsigned char GLubyte, GLboolean;
typedef char GLchar;

#define FNS(X) \
	X(EGL, EGLDisplay, eglGetDisplay, (void *)) \
	X(EGL, EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *)) \
	X(EGL, EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)) \
	X(EGL, EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, uintptr_t, const EGLint *)) \
	X(EGL, EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *)) \
	X(EGL, EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
	X(EGL, EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
	X(EGL, EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface)) \
	X(EGL, EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *)) \
	X(EGL, EGLBoolean, eglTerminate, (EGLDisplay)) \
	X(GL, GLuint, glCreateShader, (GLenum)) \
	X(GL, void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	X(GL, void, glCompileShader, (GLuint)) \
	X(GL, void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
	X(GL, void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(GL, GLuint, glCreateProgram, (void)) \
	X(GL, void, glAttachShader, (GLuint, GLuint)) \
	X(GL, void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
	X(GL, void, glLinkProgram, (GLuint)) \
	X(GL, void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
	X(GL, void, glUseProgram, (GLuint)) \
	X(GL, GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
	X(GL, void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(GL, void, glUniform1i, (GLint, GLint)) \
	X(GL, void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(GL, void, glClearDepthf, (GLfloat)) \
	X(GL, void, glClearStencil, (GLint)) \
	X(GL, void, glClear, (GLbitfield)) \
	X(GL, void, glEnable, (GLenum)) \
	X(GL, void, glDisable, (GLenum)) \
	X(GL, void, glDepthFunc, (GLenum)) \
	X(GL, void, glDepthMask, (GLboolean)) \
	X(GL, void, glDepthRangef, (GLfloat, GLfloat)) \
	X(GL, void, glStencilFunc, (GLenum, GLint, GLuint)) \
	X(GL, void, glStencilOp, (GLenum, GLenum, GLenum)) \
	X(GL, void, glStencilMask, (GLuint)) \
	X(GL, void, glBlendFunc, (GLenum, GLenum)) \
	X(GL, void, glBlendEquation, (GLenum)) \
	X(GL, void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(GL, void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
	X(GL, void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
	X(GL, void, glCullFace, (GLenum)) \
	X(GL, void, glFrontFace, (GLenum)) \
	X(GL, void, glPolygonOffset, (GLfloat, GLfloat)) \
	X(GL, void, glLineWidth, (GLfloat)) \
	X(GL, void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
	X(GL, void, glEnableVertexAttribArray, (GLuint)) \
	X(GL, void, glDisableVertexAttribArray, (GLuint)) \
	X(GL, void, glDrawArrays, (GLenum, GLint, GLsizei)) \
	X(GL, void, glFinish, (void)) \
	X(GL, GLenum, glGetError, (void)) \
	X(GL, void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
	X(GL, void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
	X(GL, void, glGenTextures, (GLsizei, GLuint *)) \
	X(GL, void, glBindTexture, (GLenum, GLuint)) \
	X(GL, void, glActiveTexture, (GLenum)) \
	X(GL, void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
	X(GL, void, glTexParameteri, (GLenum, GLenum, GLint)) \
	X(GL, void, glGenerateMipmap, (GLenum)) \
	X(GL, void, glGenFramebuffers, (GLsizei, GLuint *)) \
	X(GL, void, glBindFramebuffer, (GLenum, GLuint)) \
	X(GL, void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
	X(GL, void, glGenRenderbuffers, (GLsizei, GLuint *)) \
	X(GL, void, glBindRenderbuffer, (GLenum, GLuint)) \
	X(GL, void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
	X(GL, void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
	X(GL, GLenum, glCheckFramebufferStatus, (GLenum))

#define DECL(lib, ret, name, args) static ret(*name) args;
FNS(DECL)

#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_NONE 0x3038
#define EGL_ALPHA_SIZE 0x3021
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_DITHER 0x0BD0
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_FLOAT 0x1406
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_ALPHA 0x1906
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_REPEAT 0x2901
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_STENCIL_INDEX8 0x8D48
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5

static void (*mark)(const char *);
static GLuint fbo, fbotex, sprog;
static float quad[16];
static void (*snapshot)(void);

static void note(const char *s)
{
	fprintf(stderr, "probe: %s\n", s);
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
	while (n < max) {
		float f = strtof(s, &e);
		if (e == s)
			break;
		v[n++] = f;
		s = e;
	}
	return n;
}

/* integers in any base (GL enums are easiest as 0x...); returns how many were given */
static int ints(const char *env, long *v, int max, const char *def)
{
	const char *s = getenv(env);
	int n = 0;
	char *e;

	if (!s)
		s = def;
	if (!s)
		return 0;
	while (n < max) {
		long x = strtol(s, &e, 0);
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
	void *gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
	void *lib;

	if (!egl || !gl) {
		fprintf(stderr, "probe: dlopen: %s\n", dlerror());
		return 0;
	}
#define LOAD(l, ret, name, args) \
	lib = l; \
	if (!(*(void **)&name = dlsym(lib, #name))) { \
		fprintf(stderr, "probe: no %s\n", #name); \
		return 0; \
	}
#define EGL egl
#define GL gl
	FNS(LOAD)
#undef EGL
#undef GL
	return 1;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	char log[512];

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "probe: shader: %s\n", log);
		exit(2);
	}
	return s;
}

/* the 8x8 checker in the requested texel format: (255,128,0,255) / (0,128,255,255) */
static int texels(const char *fmt, int w, int h, void *out, GLenum *gfmt, GLenum *gtype)
{
	unsigned char *b = out;
	uint16_t *s = out;

	for (int i = 0; i < w * h; i++) {
		int on = ((i % w / 1) ^ (i / w)) & 1;
		int r = on ? 255 : 0, g = 128, bl = on ? 0 : 255, a = 255;

		if (!strcmp(fmt, "rgba8888")) {
			*gfmt = GL_RGBA, *gtype = GL_UNSIGNED_BYTE;
			b[4 * i] = r, b[4 * i + 1] = g, b[4 * i + 2] = bl, b[4 * i + 3] = a;
		} else if (!strcmp(fmt, "rgb888")) {
			*gfmt = GL_RGB, *gtype = GL_UNSIGNED_BYTE;
			b[3 * i] = r, b[3 * i + 1] = g, b[3 * i + 2] = bl;
		} else if (!strcmp(fmt, "rgb565")) {
			*gfmt = GL_RGB, *gtype = GL_UNSIGNED_SHORT_5_6_5;
			s[i] = (r >> 3) << 11 | (g >> 2) << 5 | bl >> 3;
		} else if (!strcmp(fmt, "rgba4444")) {
			*gfmt = GL_RGBA, *gtype = GL_UNSIGNED_SHORT_4_4_4_4;
			s[i] = (r >> 4) << 12 | (g >> 4) << 8 | (bl >> 4) << 4 | a >> 4;
		} else if (!strcmp(fmt, "rgba5551")) {
			*gfmt = GL_RGBA, *gtype = GL_UNSIGNED_SHORT_5_5_5_1;
			s[i] = (r >> 3) << 11 | (g >> 3) << 6 | (bl >> 3) << 1 | a >> 7;
		} else if (!strcmp(fmt, "l8")) {
			*gfmt = GL_LUMINANCE, *gtype = GL_UNSIGNED_BYTE;
			b[i] = r;
		} else if (!strcmp(fmt, "a8")) {
			*gfmt = GL_ALPHA, *gtype = GL_UNSIGNED_BYTE;
			b[i] = a;
		} else if (!strcmp(fmt, "la88")) {
			*gfmt = GL_LUMINANCE_ALPHA, *gtype = GL_UNSIGNED_BYTE;
			b[2 * i] = r, b[2 * i + 1] = a;
		} else {
			return 0;
		}
	}
	return 1;
}

/* PROBE_TEXPAT: every texel names its own coordinates, level and face (see the header) */
static int pattern(const char *fmt, int w, int h, int level, int face, void *out, GLenum *gfmt, GLenum *gtype)
{
	unsigned char *b = out;
	uint16_t *s = out;

	if (!texels(fmt, 1, 1, out, gfmt, gtype))
		return 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int i = y * w + x;
			unsigned char r = (unsigned char)x, g = (unsigned char)y, bl = (unsigned char)(level | face << 4),
				      a = (unsigned char)(0x80 | x >> 8 | (y >> 8) << 2);

			if (!strcmp(fmt, "rgba8888"))
				b[4 * i] = r, b[4 * i + 1] = g, b[4 * i + 2] = bl, b[4 * i + 3] = a;
			else if (!strcmp(fmt, "rgb888"))
				b[3 * i] = r, b[3 * i + 1] = g, b[3 * i + 2] = bl;
			else if (!strcmp(fmt, "l8") || !strcmp(fmt, "a8"))
				b[i] = (unsigned char)i;
			else
				s[i] = (uint16_t)i;	/* 565, 4444, 5551, la88 (l = low byte) */
		}
	return 1;
}

static int texbytes(GLenum gf, GLenum gt)
{
	if (gt != GL_UNSIGNED_BYTE)
		return 2;
	return gf == GL_RGBA ? 4 : gf == GL_RGB ? 3 : gf == GL_LUMINANCE_ALPHA ? 2 : 1;
}

/* fill tex with level `level` of face `face` of a w x h texture: pattern, file or checker */
static int teximage(const char *fmt, int w, int h, int level, int face, void *tex, GLenum *gf, GLenum *gt)
{
	const char *file = getenv("PROBE_TEXFILE");
	int lw = w >> level > 0 ? w >> level : 1, lh = h >> level > 0 ? h >> level : 1;

	if (getenv("PROBE_TEXPAT") && atoi(getenv("PROBE_TEXPAT"))) {
		if (!pattern(fmt, lw, lh, level, face, tex, gf, gt))
			return 0;
	} else if (!texels(fmt, lw, lh, tex, gf, gt)) {
		return 0;
	}
	if (file && !level && !face) {
		FILE *fp = fopen(file, "rb");
		size_t n = (size_t)lw * lh * texbytes(*gf, *gt);

		if (!fp || fread(tex, 1, n, fp) != n)
			return note("short PROBE_TEXFILE"), 0;
		fclose(fp);
	}
	return 1;
}

/* PROBE_FBO: a w x h colour texture (and PROBE_FBODS renderbuffers) bound as the draw target */
static int fbo_setup(void)
{
	long v[3];
	GLenum gf = GL_RGBA, gt = GL_UNSIGNED_BYTE;
	GLuint rb[2];
	long ds[2] = { 0, 0 };
	const char *fmt = getenv("PROBE_FBO");
	static unsigned char one[4];
	char name[16];

	if (ints("PROBE_FBO", v, 2, NULL) != 2 || sscanf(fmt, "%*s %*s %15s", name) != 1)
		return note("PROBE_FBO needs w h fmt"), 0;
	if (!texels(name, 1, 1, one, &gf, &gt))
		return note("bad PROBE_FBO format"), 0;
	glGenTextures(1, &fbotex);
	glBindTexture(GL_TEXTURE_2D, fbotex);
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)gf, (GLsizei)v[0], (GLsizei)v[1], 0, gf, gt, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);	/* CLAMP_TO_EDGE: NPOT targets */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbotex, 0);
	ints("PROBE_FBODS", ds, 2, NULL);
	glGenRenderbuffers(2, rb);
	if (ds[0] && ds[1]) {
		glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, (GLsizei)v[0], (GLsizei)v[1]);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[0]);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[0]);
	} else if (ds[0]) {
		glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, (GLsizei)v[0], (GLsizei)v[1]);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[0]);
	} else if (ds[1]) {
		glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, (GLsizei)v[0], (GLsizei)v[1]);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return note("FBO incomplete"), 0;
	glViewport(0, 0, (GLsizei)v[0], (GLsizei)v[1]);
	sprog = glCreateProgram();
	glAttachShader(sprog, shader(GL_VERTEX_SHADER, "attribute vec4 p; attribute vec2 a1; varying vec2 v; "
				    "void main() { gl_Position = p; v = a1; }"));
	glAttachShader(sprog, shader(GL_FRAGMENT_SHADER, "precision mediump float; uniform sampler2D t0; varying vec2 v; "
				    "void main() { gl_FragColor = texture2D(t0, v); }"));
	glBindAttribLocation(sprog, 0, "p");
	glBindAttribLocation(sprog, 1, "a1");
	glLinkProgram(sprog);
	{
		float q[4];

		floats("PROBE_QUAD", q, 4, "-0.75 -0.75 0.75 0.75");
		/* strip: x, y, u, v */
		float qq[16] = { q[0], q[1], 0, 0, q[2], q[1], 1, 0, q[0], q[3], 0, 1, q[2], q[3], 1, 1 };

		memcpy(quad, qq, sizeof qq);
	}
	return 1;
}

/* the window pass: clear, then the quad sampling the FBO texture */
static void fbo_sample(int w, int h, GLuint prog)
{
	float c[4];
	GLint loc;

	note("fbo sample");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, w, h);
	floats("PROBE_FBOCLEAR", c, 4, "0.2 0.2 0.2 1");
	glClearColor(c[0], c[1], c[2], c[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(sprog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, fbotex);
	if ((loc = glGetUniformLocation(sprog, "t0")) >= 0)
		glUniform1i(loc, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 16, quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 16, quad + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(prog);
}

int main(void)
{
	static float verts[3 * 3 * 64], inter[3 * 64 * 32], extra[8][3 * 64 * 4];
	static unsigned char tex[1024 * 1024 * 4];
	float asz[8], f[8], z = 0;
	long v[8];
	int na, ntex, stride, il, pos, nz;
	float clear[4], color[4];
	EGLint maj, min, n, w = 0, h = 0, conf[6] = { 5, 6, 5, 0, 0, 0 };
	EGLConfig cfg;
	EGLint cfg_attr[] = { EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_ALPHA_SIZE, 0,
			      EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const char *vs = getenv("PROBE_VS"), *fs = getenv("PROBE_FS"), *out = getenv("PROBE_OUT");
	const char *texfmt = getenv("PROBE_TEXFMT") ? getenv("PROBE_TEXFMT") : "rgba8888";
	int nv, draws = getenv("PROBE_DRAWS") ? atoi(getenv("PROBE_DRAWS")) : 1;
	int frames = getenv("PROBE_FRAMES") ? atoi(getenv("PROBE_FRAMES")) : 1;
	GLenum prim = getenv("PROBE_PRIM") ? (GLenum)atoi(getenv("PROBE_PRIM")) : 4;
	GLbitfield clearbits = GL_COLOR_BUFFER_BIT;
	EGLDisplay dpy;
	EGLSurface surf;
	EGLContext ctx;
	GLuint prog;
	GLint linked = 0, loc;
	unsigned char px[4];

	mark = (void (*)(const char *))dlsym(RTLD_DEFAULT, "pvrtrace_note");
	snapshot = (void (*)(void))dlsym(RTLD_DEFAULT, "pvrtrace_snapshot");
	if (!load())
		return 1;
	floats("PROBE_CLEAR", clear, 4, "0 0 0.5 1");
	floats("PROBE_COLOR", color, 4, "0 1 0 1");
	nz = floats("PROBE_Z", &z, 1, "");
	pos = nz ? 3 : 2;
	nv = floats("PROBE_VERTS", verts, 3 * 2 * 64, "-0.5 -0.5 0.5 -0.5 0 0.5") / 2;
	na = floats("PROBE_ATTRS", asz, 8, "");
	ntex = getenv("PROBE_TEX") ? atoi(getenv("PROBE_TEX")) : 0;
	il = getenv("PROBE_INTERLEAVE") && atoi(getenv("PROBE_INTERLEAVE"));
	if (!vs)
		vs = "attribute vec4 p; void main() { gl_Position = p; }";
	if (!fs)
		fs = "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }";

	note("egl init");
	for (int i = 0, k = ints("PROBE_CONFIG", v, 6, NULL); i < k; i++)
		conf[i] = (EGLint)v[i];
	for (int i = 0; i < 6; i++)
		cfg_attr[2 * i + 1] = conf[i];
	if (getenv("PROBE_PBUFFER"))
		cfg_attr[13] = EGL_PBUFFER_BIT;
	dpy = eglGetDisplay(NULL);
	if (!eglInitialize(dpy, &maj, &min) || !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1)
		return note("egl failed"), 1;
	if (ints("PROBE_PBUFFER", v, 2, NULL) == 2) {
		const EGLint pb[] = { EGL_WIDTH, (EGLint)v[0], EGL_HEIGHT, (EGLint)v[1], EGL_NONE };

		surf = eglCreatePbufferSurface(dpy, cfg, pb);
	} else {
		surf = eglCreateWindowSurface(dpy, cfg, 0, NULL);
	}
	ctx = eglCreateContext(dpy, cfg, NULL, ctx_attr);
	if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx))
		return note("surface/context failed"), 1;
	eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

	note("compile");
	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	for (int a = 0; a < na; a++) {
		char name[8];

		snprintf(name, sizeof name, "a%d", a + 1);
		glBindAttribLocation(prog, a + 1, name);
	}
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked)
		return note("link failed"), 1;
	glUseProgram(prog);
	loc = glGetUniformLocation(prog, "c");
	if (loc >= 0)
		glUniform4f(loc, color[0], color[1], color[2], color[3]);
	if (floats("PROBE_VIEWPORT", f, 4, "") == 4)
		glViewport((GLint)f[0], (GLint)f[1], (GLsizei)f[2], (GLsizei)f[3]);
	else
		glViewport(0, 0, w, h);

	note("state");
	if (ints("PROBE_DEPTH", v, 2, NULL) >= 1) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc((GLenum)v[0]);
		if (ints("PROBE_DEPTH", v, 2, NULL) == 2)
			glDepthMask((GLboolean)v[1]);
	}
	if (floats("PROBE_CLEARDEPTH", f, 1, "") == 1) {
		glClearDepthf(f[0]);
		clearbits |= GL_DEPTH_BUFFER_BIT;
	}
	if (floats("PROBE_DEPTHRANGE", f, 2, "") == 2)
		glDepthRangef(f[0], f[1]);
	if (ints("PROBE_STENCIL", v, 7, NULL) == 7) {
		glEnable(GL_STENCIL_TEST);
		glStencilFunc((GLenum)v[0], (GLint)v[1], (GLuint)v[2]);
		glStencilOp((GLenum)v[3], (GLenum)v[4], (GLenum)v[5]);
		glStencilMask((GLuint)v[6]);
	}
	if (ints("PROBE_CLEARSTENCIL", v, 1, NULL) == 1) {
		glClearStencil((GLint)v[0]);
		clearbits |= GL_STENCIL_BUFFER_BIT;
	}
	if ((n = ints("PROBE_BLEND", v, 3, NULL)) >= 2) {
		glEnable(GL_BLEND);
		glBlendFunc((GLenum)v[0], (GLenum)v[1]);
		if (n == 3)
			glBlendEquation((GLenum)v[2]);
	}
	if (floats("PROBE_BLENDCOLOR", f, 4, "") == 4)
		glBlendColor(f[0], f[1], f[2], f[3]);
	if (ints("PROBE_CMASK", v, 4, NULL) == 4)
		glColorMask((GLboolean)v[0], (GLboolean)v[1], (GLboolean)v[2], (GLboolean)v[3]);
	if (ints("PROBE_SCISSOR", v, 4, NULL) == 4) {
		glEnable(GL_SCISSOR_TEST);
		glScissor((GLint)v[0], (GLint)v[1], (GLsizei)v[2], (GLsizei)v[3]);
	}
	if (ints("PROBE_CULL", v, 2, NULL) >= 1) {
		glEnable(GL_CULL_FACE);
		glCullFace((GLenum)v[0]);
		if (ints("PROBE_CULL", v, 2, NULL) == 2)
			glFrontFace((GLenum)v[1]);
	}
	if (floats("PROBE_POLYOFF", f, 2, "") == 2) {
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(f[0], f[1]);
	}
	if (getenv("PROBE_DITHER") && !atoi(getenv("PROBE_DITHER")))
		glDisable(GL_DITHER);
	if (floats("PROBE_LINEWIDTH", f, 1, "") == 1)
		glLineWidth(f[0]);

	for (int t = 0; t < ntex; t++) {
		GLuint tx;
		GLenum gf, gt;
		char name[8];
		long tw = 8, th = 8, filt[2] = { GL_NEAREST, GL_NEAREST }, wrap[2] = { GL_REPEAT, GL_REPEAT };

		if (ints("PROBE_TEXSIZE", v, 2, NULL) == 2)
			tw = v[0], th = v[1];
		ints("PROBE_TEXFILTER", filt, 2, NULL);
		ints("PROBE_TEXWRAP", wrap, 2, NULL);
		int cube = getenv("PROBE_TEXCUBE") && atoi(getenv("PROBE_TEXCUBE"));
		int levels = getenv("PROBE_TEXLEVELS") ? atoi(getenv("PROBE_TEXLEVELS")) : 1;
		GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;

		if (tw * th > 1024 * 1024)
			return note("bad texture size"), 1;
		glActiveTexture(GL_TEXTURE0 + t);
		glGenTextures(1, &tx);
		glBindTexture(target, tx);
		for (int fc = 0; fc < (cube ? 6 : 1); fc++)
			for (int l = 0; l < levels; l++) {
				GLsizei lw = tw >> l > 0 ? (GLsizei)(tw >> l) : 1, lh = th >> l > 0 ? (GLsizei)(th >> l) : 1;

				if (!teximage(texfmt, (int)tw, (int)th, l, fc, tex, &gf, &gt))
					return note("bad texture format"), 1;
				glTexImage2D(cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)fc : GL_TEXTURE_2D, l, (GLint)gf,
					     lw, lh, 0, gf, gt, tex);
			}
		if (getenv("PROBE_MIPMAP") && atoi(getenv("PROBE_MIPMAP")))
			glGenerateMipmap(target);
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, (GLint)filt[0]);
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, (GLint)filt[1]);
		glTexParameteri(target, GL_TEXTURE_WRAP_S, (GLint)wrap[0]);
		glTexParameteri(target, GL_TEXTURE_WRAP_T, (GLint)wrap[1]);
		snprintf(name, sizeof name, "t%d", t);
		loc = glGetUniformLocation(prog, name);
		if (loc >= 0)
			glUniform1i(loc, t);
	}

	if (getenv("PROBE_FBO") && !fbo_setup())
		return 1;
	/* extra attribute a<k>, vertex v, component c = 0.25 * (k + 1) + 0.125 * v + 0.0625 * c */
	stride = pos;
	for (int a = 0; a < na; a++)
		stride += (int)asz[a];
	for (int i = nv - 1; i >= 0; i--) {
		float x = verts[2 * i], y = verts[2 * i + 1];

		verts[pos * i] = x, verts[pos * i + 1] = y;
		if (nz)
			verts[pos * i + 2] = z;
	}
	for (int i = 0; i < nv; i++) {
		int o = i * stride;

		for (int c = 0; c < pos; c++)
			inter[o + c] = verts[pos * i + c];
		o += pos;
		for (int a = 0; a < na; a++)
			for (int c = 0; c < (int)asz[a]; c++, o++)
				inter[o] = extra[a][i * (int)asz[a] + c] = 0.25f * (a + 1) + 0.125f * i + 0.0625f * c;
	}
	if (il) {
		int off = pos;

		glVertexAttribPointer(0, pos, GL_FLOAT, 0, stride * 4, inter);
		for (int a = 0; a < na; a++) {
			glVertexAttribPointer(a + 1, (int)asz[a], GL_FLOAT, 0, stride * 4, inter + off);
			off += (int)asz[a];
		}
	} else {
		glVertexAttribPointer(0, pos, GL_FLOAT, 0, 0, verts);
		for (int a = 0; a < na; a++)
			glVertexAttribPointer(a + 1, (int)asz[a], GL_FLOAT, 0, 0, extra[a]);
	}
	for (int a = 0; a <= na; a++)
		glEnableVertexAttribArray(a);
	for (int fr = 0; fr < frames; fr++) {
	/* PROBE_FRAMES: frame fr clears to the clear colour with red = fr / frames */
	if (frames > 1)
		clear[0] = (float)fr / frames;
	note("clear");
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	if (!getenv("PROBE_NOCLEAR"))
		glClear(clearbits);
	for (int i = 0; i < draws; i++) {
		if (i == 1 && ints("PROBE_SCISSOR2", v, 4, NULL) == 4)
			glScissor((GLint)v[0], (GLint)v[1], (GLsizei)v[2], (GLsizei)v[3]);
		if (i == 1 && getenv("PROBE_NOSCISSOR2"))
			glDisable(GL_SCISSOR_TEST);
		if (i == 1 && floats("PROBE_VIEWPORT2", f, 4, "") == 4)
			glViewport((GLint)f[0], (GLint)f[1], (GLsizei)f[2], (GLsizei)f[3]);
		note("draw");
		glDrawArrays(prim, 0, nv);
	}
	if (fbo) {
		static float re[3 * 3];
		float r[11];

		fbo_sample(w, h, prog);
		if (floats("PROBE_FBOREENTER", r, 11, "") == 11) {
			long fv[2];

			note("fbo reenter");
			ints("PROBE_FBO", fv, 2, NULL);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glViewport(0, 0, (GLsizei)fv[0], (GLsizei)fv[1]);
			for (int i = 0; i < 3; i++)
				re[3 * i] = r[2 * i], re[3 * i + 1] = r[2 * i + 1], re[3 * i + 2] = r[6];
			if ((loc = glGetUniformLocation(prog, "c")) >= 0)
				glUniform4f(loc, r[7], r[8], r[9], r[10]);
			glDisableVertexAttribArray(1);
			glVertexAttribPointer(0, 3, GL_FLOAT, 0, 0, re);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			fbo_sample(w, h, prog);
		}
	}
	if (fr < frames - 1) {
		note("swap");
		eglSwapBuffers(dpy, surf);
		continue;
	}
	note("finish");
	glFinish();
	if (snapshot)
		snapshot();	/* the finished frame, as the replay's reference */
	note("readback");
	glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("probe: %dx%d centre rgba(%d,%d,%d,%d) glerr %#x\n", w, h, px[0], px[1], px[2], px[3],
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
	}
	note("terminate");
	eglTerminate(dpy);
	return 0;
}
