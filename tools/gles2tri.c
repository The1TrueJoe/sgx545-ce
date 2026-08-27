/*
 * gles2tri — the actual question: does this board do 3D?
 *
 * Brings up EGL on the framebuffer, compiles a shader pair, draws a triangle
 * and reads the result back out of the GPU. Every stage is reported, because
 * each one is a different part of the stack and they fail independently:
 *
 *   eglInitialize      -> libIMGegl + libsrv_um talking to the kernel driver
 *   eglCreateWindowSurface -> the WSEGL backend against /dev/fb0
 *   eglCreateContext   -> services connected, microkernel accepted
 *   glCreateShader/glCompileShader -> libglslcompiler + libusc, the USSE
 *                         shader compiler -- the piece nobody has ever
 *                         reimplemented
 *   glDrawArrays + glReadPixels -> the SGX actually rasterised something
 *
 * GL_RENDERER is the headline. If that says PowerVR SGX and the pixel comes
 * back the colour we drew, the GPU rendered it.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define STEP(msg) do { printf("  %-38s", msg); fflush(stdout); } while (0)
#define OK()      printf("ok\n")
#define FAILED(x) do { printf("FAILED (%s)\n", x); return 1; } while (0)

static const char *vs =
	"attribute vec4 p;\n"
	"void main() { gl_Position = p; }\n";

static const char *fs =
	"precision mediump float;\n"
	"void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

static GLuint compile(GLenum type, const char *src, const char *what)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = {0};
		glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
		printf("FAILED\n    %s shader: %s\n", what, log);
		return 0;
	}
	return s;
}

int main(void)
{
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLContext ctx;
	EGLint n, maj, min;
	GLuint v, f, prog;
	GLint linked;
	unsigned char px[4] = {0, 0, 0, 0};
	static const EGLint cfg_attr[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const GLfloat tri[] = { 0.0f, 0.8f,  -0.8f, -0.8f,  0.8f, -0.8f };

	printf("gles2tri: EGL/GLES2 on the SGX545\n");

	STEP("eglGetDisplay");
	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY) FAILED("no display");
	OK();

	STEP("eglInitialize");
	if (!eglInitialize(dpy, &maj, &min)) FAILED("services/driver not reachable");
	printf("\r  eglInitialize                         ok (EGL %d.%d)\n", maj, min);

	printf("    EGL_VENDOR  : %s\n", eglQueryString(dpy, EGL_VENDOR));
	printf("    EGL_VERSION : %s\n", eglQueryString(dpy, EGL_VERSION));

	STEP("eglChooseConfig");
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) FAILED("no config");
	OK();

	STEP("eglCreateWindowSurface (fbdev)");
	surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
	if (surf == EGL_NO_SURFACE) FAILED("WSEGL backend refused /dev/fb0");
	OK();

	STEP("eglCreateContext");
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT) FAILED("no ES2 context");
	OK();

	STEP("eglMakeCurrent");
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) FAILED("makecurrent");
	OK();

	printf("    GL_VENDOR   : %s\n", glGetString(GL_VENDOR));
	printf("    GL_RENDERER : %s\n", glGetString(GL_RENDERER));
	printf("    GL_VERSION  : %s\n", glGetString(GL_VERSION));

	STEP("compile shaders (USSE compiler)");
	v = compile(GL_VERTEX_SHADER, vs, "vertex");
	if (!v) return 1;
	f = compile(GL_FRAGMENT_SHADER, fs, "fragment");
	if (!f) return 1;
	OK();

	STEP("link program");
	prog = glCreateProgram();
	glAttachShader(prog, v);
	glAttachShader(prog, f);
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[1024] = {0};
		glGetProgramInfoLog(prog, sizeof log - 1, NULL, log);
		printf("FAILED\n    %s\n", log);
		return 1;
	}
	OK();

	STEP("draw a triangle");
	glClearColor(0.0f, 0.0f, 0.5f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(prog);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	if (glGetError() != GL_NO_ERROR) FAILED("GL error during draw");
	OK();

	/* Centre of the surface is inside the triangle, so it must be the green
	 * the fragment shader writes -- not the clear colour. That is the
	 * difference between "the API worked" and "the GPU rasterised". */
	STEP("read back the centre pixel");
	glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("got rgba(%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);

	STEP("eglSwapBuffers (to the panel)");
	if (!eglSwapBuffers(dpy, surf)) FAILED("swap");
	OK();

	printf("\n  the SGX545 rendered a frame.\n");
	eglDestroyContext(dpy, ctx);
	eglDestroySurface(dpy, surf);
	eglTerminate(dpy);
	return 0;
}
