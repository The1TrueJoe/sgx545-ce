/*
 * gpubench — is the SGX545 worth using for a launcher UI on this board?
 *
 * Not a general GPU benchmark. It measures the four things a set-top launcher
 * actually does, at this panel's real resolution, and prints each as "layers of
 * full-screen overdraw you could afford at 60 fps" — because that is the number
 * that decides a UI design, not MPixels/s in the abstract.
 *
 *   fill        flat quads             backgrounds, solid panels
 *   blend       alpha-blended quads    stacked cards, scrims, fades
 *   texture     textured + blended     icons, artwork, glyph atlases
 *   upload      glTexSubImage2D        pushing new artwork in each frame
 *
 * and a CPU memcpy-to-framebuffer baseline, because "offload it to the GPU" is
 * only worth doing if the GPU is actually the faster path. On a 720x480 panel
 * against a 1.2 GHz Atom that is a real question, not a formality.
 *
 * Deliberately no video: this models a launcher, per what it is for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static const char *vs_src =
	"attribute vec2 p;\n"
	"varying vec2 uv;\n"
	"void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *fs_flat =
	"precision mediump float;\n"
	"uniform vec4 c;\n"
	"void main() { gl_FragColor = c; }\n";
static const char *fs_tex =
	"precision mediump float;\n"
	"varying vec2 uv;\n"
	"uniform sampler2D t;\n"
	"uniform vec4 c;\n"
	"void main() { gl_FragColor = texture2D(t, uv) * c; }\n";

static GLuint prog(const char *vs, const char *fs)
{
	GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
	GLint ok;
	glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
	glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
	if (!ok) { char l[512]={0}; glGetShaderInfoLog(v,511,NULL,l); fprintf(stderr,"vs: %s\n",l); return 0; }
	glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
	glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
	if (!ok) { char l[512]={0}; glGetShaderInfoLog(f,511,NULL,l); fprintf(stderr,"fs: %s\n",l); return 0; }
	p = glCreateProgram();
	glAttachShader(p, v); glAttachShader(p, f);
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) { char l[512]={0}; glGetProgramInfoLog(p,511,NULL,l); fprintf(stderr,"link: %s\n",l); return 0; }
	return p;
}

/* Report as overdraw-at-60fps: how many full-screen layers fit in a frame. */
static void report(const char *what, double mpix_s, int w, int h)
{
	double per_frame = mpix_s * 1e6 / 60.0;
	printf("  %-9s %8.1f Mpix/s   %5.1f full-screen layers at 60 fps\n",
	       what, mpix_s, per_frame / (double)(w * h));
}

int main(void)
{
	EGLDisplay dpy; EGLConfig cfg; EGLSurface surf; EGLContext ctx;
	EGLint n, maj, min;
	static const EGLint ca[] = { EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const GLfloat quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
	int W = 720, H = 480, i, iters;
	double t0, dt;
	GLuint pflat, ptex, tex;

	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
	if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1) { fprintf(stderr, "no config\n"); return 1; }
	surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
	if (surf == EGL_NO_SURFACE) { fprintf(stderr, "no surface\n"); return 1; }
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
	eglMakeCurrent(dpy, surf, surf, ctx);
	eglQuerySurface(dpy, surf, EGL_WIDTH, &n);  if (n > 0) W = n;
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &n); if (n > 0) H = n;

	printf("gpubench: %s\n", glGetString(GL_RENDERER));
	printf("  surface %dx%d\n\n", W, H);

	pflat = prog(vs_src, fs_flat);
	ptex  = prog(vs_src, fs_tex);
	if (!pflat || !ptex) return 1;
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, W, H);

	/* 1. flat fill */
	iters = 300;
	glUseProgram(pflat);
	glUniform4f(glGetUniformLocation(pflat, "c"), 0.2f, 0.4f, 0.8f, 1.0f);
	glDisable(GL_BLEND);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish();     /* warm */
	t0 = now();
	for (i = 0; i < iters; i++) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish(); dt = now() - t0;
	report("fill", (double)W * H * iters / dt / 1e6, W, H);

	/* 2. alpha-blended fill — stacked UI layers */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUniform4f(glGetUniformLocation(pflat, "c"), 0.2f, 0.4f, 0.8f, 0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish();
	t0 = now();
	for (i = 0; i < iters; i++) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish(); dt = now() - t0;
	report("blend", (double)W * H * iters / dt / 1e6, W, H);

	/* 3. textured + blended — icons and artwork */
	{
		unsigned char *px = malloc(256 * 256 * 4);
		for (i = 0; i < 256 * 256; i++) {
			px[i*4+0] = i & 0xff; px[i*4+1] = (i >> 8) & 0xff;
			px[i*4+2] = 0x80;     px[i*4+3] = 0xc0;
		}
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glUseProgram(ptex);
		glUniform1i(glGetUniformLocation(ptex, "t"), 0);
		glUniform4f(glGetUniformLocation(ptex, "c"), 1, 1, 1, 1);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish();
		t0 = now();
		for (i = 0; i < iters; i++) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFinish(); dt = now() - t0;
		report("texture", (double)W * H * iters / dt / 1e6, W, H);

		/* 4. texture upload — new artwork per frame */
		iters = 200;
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glFinish();
		t0 = now();
		for (i = 0; i < iters; i++)
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glFinish(); dt = now() - t0;
		printf("  %-9s %8.1f MB/s    (256x256 RGBA, %d/s)\n", "upload",
		       256.0 * 256 * 4 * iters / dt / 1048576.0, (int)(iters / dt));
		free(px);
	}

	/* 5. CPU baseline: the thing we would be offloading FROM. */
	{
		int fd = open("/dev/fb0", O_RDWR);
		if (fd >= 0) {
			struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
			if (!ioctl(fd, FBIOGET_VSCREENINFO, &v) && !ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
				size_t len = (size_t)f.line_length * v.yres;
				void *fb = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
				void *src = malloc(len);
				if (fb != MAP_FAILED && src) {
					memset(src, 0x40, len);
					memcpy(fb, src, len);           /* warm */
					iters = 120;
					t0 = now();
					for (i = 0; i < iters; i++) memcpy(fb, src, len);
					dt = now() - t0;
					printf("\n  %-9s %8.1f Mpix/s   %5.1f full-screen layers at 60 fps  <- CPU memcpy\n",
					       "cpu", (double)v.xres * v.yres * iters / dt / 1e6,
					       ((double)v.xres * v.yres * iters / dt) / 60.0 / (v.xres * v.yres));
					munmap(fb, len);
				}
				free(src);
			}
			close(fd);
		}
	}

	printf("\n  a launcher wants roughly 2-4 layers of overdraw at 60 fps.\n");
	eglDestroyContext(dpy, ctx); eglDestroySurface(dpy, surf); eglTerminate(dpy);
	return 0;
}
