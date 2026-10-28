/*
 * glt: draw a list of GLES2 test programs into an RGBA8888 pbuffer with the
 * DDK and save each frame.
 *
 *   glt <list> <outdir>
 *
 * The list holds blocks of lines:
 *   test <name>                    start a test (its shaders are tagged
 *                                  <name>.vert / <name>.frag for uspsub.so)
 *   vs <path> / fs <path>          shader sources
 *   u <name> f [f f f]             glUniform1f..4f
 *   m <name> f*16                  glUniformMatrix4fv
 *   i <name> n                     glUniform1i (samplers)
 *   tex <unit> <kind>              8x8 RGBA texture: grad (r = s, g = t,
 *                                  b = 0.6 - s, a = 0.8) or checker; nearest
 *   attr <name> <loc> f*16         4 components for each of the quad's 4 corners
 *   end                            draw and save <outdir>/<name>.rgba
 * Every program gets attribute p (location 0) = the corner (x, y, 0, 1) of a
 * full-surface quad (two triangles, corners (-1,-1) (1,-1) (-1,1) (1,1)).
 * EGL and GLES are loaded with dlopen (the DDK's libraries have no SONAME).
 *
 *   zig cc -target x86-linux-gnu.2.17 -O1 -o glt glt.c -ldl
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
	X(EGL, EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *)) \
	X(EGL, EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
	X(EGL, EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
	X(EGL, EGLBoolean, eglTerminate, (EGLDisplay)) \
	X(GL, GLuint, glCreateShader, (GLenum)) \
	X(GL, void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	X(GL, void, glCompileShader, (GLuint)) \
	X(GL, void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
	X(GL, void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(GL, void, glDeleteShader, (GLuint)) \
	X(GL, GLuint, glCreateProgram, (void)) \
	X(GL, void, glDeleteProgram, (GLuint)) \
	X(GL, void, glAttachShader, (GLuint, GLuint)) \
	X(GL, void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
	X(GL, void, glLinkProgram, (GLuint)) \
	X(GL, void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
	X(GL, void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(GL, void, glUseProgram, (GLuint)) \
	X(GL, GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
	X(GL, void, glUniform1f, (GLint, GLfloat)) \
	X(GL, void, glUniform2f, (GLint, GLfloat, GLfloat)) \
	X(GL, void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
	X(GL, void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(GL, void, glUniform1i, (GLint, GLint)) \
	X(GL, void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
	X(GL, void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(GL, void, glClear, (GLbitfield)) \
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
	X(GL, void, glTexParameteri, (GLenum, GLenum, GLint))

#define DECL(lib, ret, name, args) static ret(*name) args;
FNS(DECL)

#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_NONE 0x3038
#define EGL_ALPHA_SIZE 0x3021
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
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
#define GL_TRIANGLE_STRIP 5

#define W 64
#define H 64

static void (*tagfn)(const char *);

static int load(void)
{
	void *egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	void *gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
	if (!egl || !gl) {
		fprintf(stderr, "glt: dlopen: %s\n", dlerror());
		return 0;
	}
#define LOAD(lib, ret, name, args) \
	if (!(name = (ret(*) args)dlsym(lib == EGL ? egl : gl, #name))) { \
		fprintf(stderr, "glt: no %s\n", #name); \
		return 0; \
	}
	enum { EGL, GL };
	FNS(LOAD)
	tagfn = (void (*)(const char *))dlsym(RTLD_DEFAULT, "uspsub_tag");
	return 1;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *s;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	s = calloc(1, n + 1);
	if (fread(s, 1, n, f) != (size_t)n) {
		free(s);
		s = NULL;
	}
	fclose(f);
	return s;
}

static GLuint shader(GLenum type, const char *src, const char *tag)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	char log[1024] = "";
	if (tagfn)
		tagfn(tag);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "glt: %s: compile failed: %s\n", tag, log);
	}
	return s;
}

struct test {
	char name[128];
	char *vs, *fs;
	struct { char name[64]; int n; float v[16]; } u[32];
	int nu;
	struct { char name[64]; int loc; float v[16]; } a[8];
	int na;
	int tex[4];
};

static void texture(int unit, int kind)
{
	static unsigned char px[8 * 8 * 4];
	GLuint t;
	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			unsigned char *p = &px[(y * 8 + x) * 4];
			if (kind == 1) {
				p[0] = (unsigned char)(x * 32 + 16);
				p[1] = (unsigned char)(y * 32 + 16);
				p[2] = (unsigned char)(153 - x * 16);
				p[3] = 204;
			} else {
				int on = (x + y) & 1;
				p[0] = on ? 230 : 20;
				p[1] = on ? 40 : 200;
				p[2] = on ? 100 : 160;
				p[3] = on ? 255 : 128;
			}
		}
	}
	glGenTextures(1, &t);
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static int draw(struct test *t, const char *outdir)
{
	static const float corners[16] = { -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1 };
	static unsigned char px[W * H * 4];
	char tag[160], path[512];
	GLint ok = 0;
	GLuint prog = glCreateProgram(), vs, fs;

	snprintf(tag, sizeof tag, "%s.vert", t->name);
	vs = shader(GL_VERTEX_SHADER, t->vs, tag);
	snprintf(tag, sizeof tag, "%s.frag", t->name);
	fs = shader(GL_FRAGMENT_SHADER, t->fs, tag);
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "p");
	for (int i = 0; i < t->na; i++)
		glBindAttribLocation(prog, t->a[i].loc, t->a[i].name);
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetProgramInfoLog(prog, sizeof log, NULL, log);
		fprintf(stderr, "glt: %s: link failed: %s\n", t->name, log);
		return 1;
	}
	glUseProgram(prog);
	for (int i = 0; i < 4; i++)
		if (t->tex[i])
			texture(i, t->tex[i]);
	for (int i = 0; i < t->nu; i++) {
		GLint loc = glGetUniformLocation(prog, t->u[i].name);
		float *v = t->u[i].v;
		fprintf(stderr, "glt: %s: uniform %s location %d\n", t->name, t->u[i].name, loc);
		if (loc < 0)
			continue;
		switch (t->u[i].n) {
		case -1: glUniform1i(loc, (GLint)v[0]); break;
		case 1: glUniform1f(loc, v[0]); break;
		case 2: glUniform2f(loc, v[0], v[1]); break;
		case 3: glUniform3f(loc, v[0], v[1], v[2]); break;
		case 4: glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
		case 16: glUniformMatrix4fv(loc, 1, 0, v); break;
		}
	}
	glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, corners);
	glEnableVertexAttribArray(0);
	for (int i = 0; i < t->na; i++) {
		glVertexAttribPointer(t->a[i].loc, 4, GL_FLOAT, 0, 0, t->a[i].v);
		glEnableVertexAttribArray(t->a[i].loc);
	}
	glClearColor(0.2f, 0.4f, 0.6f, 0.8f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	GLenum e = glGetError();
	snprintf(path, sizeof path, "%s/%s.rgba", outdir, t->name);
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(px, 1, sizeof px, f) != sizeof px)
		perror(path);
	if (f)
		fclose(f);
	for (int i = 0; i < t->na; i++)
		glDisableVertexAttribArray(t->a[i].loc);
	glDeleteProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	fprintf(stderr, "glt: %s drawn%s\n", t->name, e ? " (GL error)" : "");
	return 0;
}

int main(int argc, char **argv)
{
	EGLint maj, min, n;
	EGLConfig cfg;
	const EGLint cfg_attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
				    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const EGLint pb[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	char line[4096];
	struct test t;
	int bad = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: glt <list> <outdir>\n");
		return 2;
	}
	if (!load())
		return 1;
	EGLDisplay dpy = eglGetDisplay(NULL);
	if (!eglInitialize(dpy, &maj, &min) || !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "glt: no RGBA8888 pbuffer config\n");
		return 1;
	}
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
	EGLContext ctx = eglCreateContext(dpy, cfg, NULL, ctx_attr);
	if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "glt: surface/context failed\n");
		return 1;
	}
	glViewport(0, 0, W, H);

	FILE *list = fopen(argv[1], "r");
	if (!list) {
		perror(argv[1]);
		return 1;
	}
	memset(&t, 0, sizeof t);
	while (fgets(line, sizeof line, list)) {
		char *k = strtok(line, " \t\n"), *a = k ? strtok(NULL, " \t\n") : NULL;
		if (!k)
			continue;
		if (!strcmp(k, "test") && a) {
			memset(&t, 0, sizeof t);
			snprintf(t.name, sizeof t.name, "%s", a);
		} else if (!strcmp(k, "vs") && a) {
			t.vs = slurp(a);
		} else if (!strcmp(k, "fs") && a) {
			t.fs = slurp(a);
		} else if ((!strcmp(k, "u") || !strcmp(k, "m") || !strcmp(k, "i")) && a && t.nu < 32) {
			int c = 0;
			char *x;
			snprintf(t.u[t.nu].name, 64, "%s", a);
			while (c < 16 && (x = strtok(NULL, " \t\n")))
				t.u[t.nu].v[c++] = strtof(x, NULL);
			t.u[t.nu].n = !strcmp(k, "i") ? -1 : !strcmp(k, "m") ? 16 : c;
			t.nu++;
		} else if (!strcmp(k, "attr") && a && t.na < 8) {
			char *x = strtok(NULL, " \t\n");
			int c = 0;
			snprintf(t.a[t.na].name, 64, "%s", a);
			t.a[t.na].loc = x ? atoi(x) : 1;
			while (c < 16 && (x = strtok(NULL, " \t\n")))
				t.a[t.na].v[c++] = strtof(x, NULL);
			t.na++;
		} else if (!strcmp(k, "tex") && a) {
			char *x = strtok(NULL, " \t\n");
			int u = atoi(a);
			if (u >= 0 && u < 4)
				t.tex[u] = x && !strcmp(x, "checker") ? 2 : 1;
		} else if (!strcmp(k, "end")) {
			if (!t.vs || !t.fs)
				fprintf(stderr, "glt: %s: missing shader\n", t.name), bad++;
			else
				bad += draw(&t, argv[2]);
			free(t.vs);
			free(t.fs);
			memset(&t, 0, sizeof t);
		}
	}
	eglTerminate(dpy);
	return bad != 0;
}
