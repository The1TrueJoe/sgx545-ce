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

static void (*mark)(const char *);

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
	static float verts[3 * 2 * 64];
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
	floats("PROBE_CLEAR", clear, 4, "0 0 0.5 1");
	floats("PROBE_COLOR", color, 4, "0 1 0 1");
	nv = floats("PROBE_VERTS", verts, (int)(sizeof verts / sizeof *verts), "-0.5 -0.5 0.5 -0.5 0 0.5") / 2;
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
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked)
		return note("link failed"), 1;
	glUseProgram(prog);
	loc = glGetUniformLocation(prog, "c");
	if (loc >= 0)
		glUniform4f(loc, color[0], color[1], color[2], color[3]);
	glViewport(0, 0, w, h);

	note("clear");
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, verts);
	glEnableVertexAttribArray(0);
	for (int i = 0; i < draws; i++) {
		note("draw");
		glDrawArrays(prim, 0, nv);
	}
	note("finish");
	glFinish();
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
