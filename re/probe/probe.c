/*
 * probe: a GLES2 program whose every input comes from the environment, so two
 * captures that differ in exactly one thing can be diffed (re/decode/diff.py).
 *
 *   PROBE_CLEAR="r g b a"      clear colour                    (0 0 0.5 1)
 *   PROBE_COLOR="r g b a"      uniform the fragment shader writes (0 1 0 1)
 *   PROBE_VERTS="x0 y0 ..."    triangle vertices, clip space, any multiple of 3
 *   PROBE_DRAWS=n              draw the vertex list n times      (1)
 *   PROBE_VS / PROBE_FS        shader source overrides
 *   PROBE_PRIM=n               GL primitive enum                 (4 = GL_TRIANGLES)
 *   PROBE_ATTRS="n1 n2 ..."    extra float attributes a1, a2, ... with n components each
 *   PROBE_INTERLEAVE=1         p and the extra attributes in one interleaved array
 *   PROBE_TEX=n                n RGBA8 8x8 checker textures, units 0..n-1, sampler uniforms t0..
 *   PROBE_OUT=path             raw RGBA readback of the whole surface
 *
 * Marks each phase in the pvrtrace log through pvrtrace_note when the tracer is loaded.
 * No DDK headers needed: the few EGL/GLES entry points are declared here.
 *   zig cc -target x86-linux-gnu.2.17 -O1 -o probe probe.c -L<ddk>/usr/lib -lEGL -lGLESv2 -ldl
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

EGLDisplay eglGetDisplay(void *);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, uintptr_t, const EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint *);
EGLBoolean eglTerminate(EGLDisplay);

GLuint glCreateShader(GLenum);
void glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
void glCompileShader(GLuint);
void glGetShaderiv(GLuint, GLenum, GLint *);
void glGetShaderInfoLog(GLuint, GLsizei, GLsizei *, GLchar *);
GLuint glCreateProgram(void);
void glAttachShader(GLuint, GLuint);
void glBindAttribLocation(GLuint, GLuint, const GLchar *);
void glLinkProgram(GLuint);
void glGetProgramiv(GLuint, GLenum, GLint *);
void glUseProgram(GLuint);
GLint glGetUniformLocation(GLuint, const GLchar *);
void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClear(GLbitfield);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
void glEnableVertexAttribArray(GLuint);
void glDrawArrays(GLenum, GLint, GLsizei);
void glFinish(void);
GLenum glGetError(void);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glGenTextures(GLsizei, GLuint *);
void glBindTexture(GLenum, GLuint);
void glActiveTexture(GLenum);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void glTexParameteri(GLenum, GLenum, GLint);
void glUniform1i(GLint, GLint);

#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600

static void (*mark)(const char *);
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

int main(void)
{
	static float verts[3 * 2 * 64], inter[3 * 64 * 32], extra[8][3 * 64 * 4];
	static unsigned char checker[8 * 8 * 4];
	float asz[8];
	int na, ntex, stride, il;
	float clear[4], color[4];
	EGLint maj, min, n, w = 0, h = 0;
	EGLConfig cfg;
	const EGLint cfg_attr[] = { EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
				    EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
				    EGL_OPENGL_ES2_BIT, EGL_NONE };
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const char *vs = getenv("PROBE_VS"), *fs = getenv("PROBE_FS"), *out = getenv("PROBE_OUT");
	int nv, draws = getenv("PROBE_DRAWS") ? atoi(getenv("PROBE_DRAWS")) : 1;
	GLenum prim = getenv("PROBE_PRIM") ? (GLenum)atoi(getenv("PROBE_PRIM")) : 4;
	EGLDisplay dpy;
	EGLSurface surf;
	EGLContext ctx;
	GLuint prog;
	GLint linked = 0, loc;
	unsigned char px[4];

	mark = (void (*)(const char *))dlsym(RTLD_DEFAULT, "pvrtrace_note");
	snapshot = (void (*)(void))dlsym(RTLD_DEFAULT, "pvrtrace_snapshot");
	floats("PROBE_CLEAR", clear, 4, "0 0 0.5 1");
	floats("PROBE_COLOR", color, 4, "0 1 0 1");
	nv = floats("PROBE_VERTS", verts, (int)(sizeof verts / sizeof *verts), "-0.5 -0.5 0.5 -0.5 0 0.5") / 2;
	na = floats("PROBE_ATTRS", asz, 8, "");
	ntex = getenv("PROBE_TEX") ? atoi(getenv("PROBE_TEX")) : 0;
	il = getenv("PROBE_INTERLEAVE") && atoi(getenv("PROBE_INTERLEAVE"));
	if (!vs)
		vs = "attribute vec4 p; void main() { gl_Position = p; }";
	if (!fs)
		fs = "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }";

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
	glViewport(0, 0, w, h);
	for (int i = 0; i < 8 * 8; i++) {
		int on = ((i & 7) ^ (i >> 3)) & 1;

		checker[4 * i] = on ? 255 : 0;
		checker[4 * i + 1] = 128;
		checker[4 * i + 2] = on ? 0 : 255;
		checker[4 * i + 3] = 255;
	}
	for (int t = 0; t < ntex; t++) {
		GLuint tex;
		char name[8];

		glActiveTexture(GL_TEXTURE0 + t);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		snprintf(name, sizeof name, "t%d", t);
		loc = glGetUniformLocation(prog, name);
		if (loc >= 0)
			glUniform1i(loc, t);
	}

	note("clear");
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	/* extra attribute a<k>, vertex v, component c = 0.25 * (k + 1) + 0.125 * v + 0.0625 * c */
	stride = 2;
	for (int a = 0; a < na; a++)
		stride += (int)asz[a];
	for (int v = 0; v < nv; v++) {
		int o = v * stride;

		inter[o] = verts[2 * v];
		inter[o + 1] = verts[2 * v + 1];
		o += 2;
		for (int a = 0; a < na; a++)
			for (int c = 0; c < (int)asz[a]; c++, o++)
				inter[o] = extra[a][v * (int)asz[a] + c] = 0.25f * (a + 1) + 0.125f * v + 0.0625f * c;
	}
	if (il) {
		int off = 2;

		glVertexAttribPointer(0, 2, GL_FLOAT, 0, stride * 4, inter);
		for (int a = 0; a < na; a++) {
			glVertexAttribPointer(a + 1, (int)asz[a], GL_FLOAT, 0, stride * 4, inter + off);
			off += (int)asz[a];
		}
	} else {
		glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, verts);
		for (int a = 0; a < na; a++)
			glVertexAttribPointer(a + 1, (int)asz[a], GL_FLOAT, 0, 0, extra[a]);
	}
	for (int a = 0; a <= na; a++)
		glEnableVertexAttribArray(a);
	for (int i = 0; i < draws; i++) {
		note("draw");
		glDrawArrays(prim, 0, nv);
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
		FILE *f;

		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
		if ((f = fopen(out, "wb"))) {
			fwrite(img, 4, (size_t)w * h, f);
			fclose(f);
		}
	}
	note("swap");
	eglSwapBuffers(dpy, surf);
	note("terminate");
	eglTerminate(dpy);
	return 0;
}
