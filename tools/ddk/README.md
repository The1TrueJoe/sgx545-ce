# tools

## sgxregs.sh
Reads the SGX identity registers on the board. Run this before anything else —
it answers "is the GPU there, clocked, and where we think it is" in a minute.
`test-sgxregs.sh` self-checks its arithmetic against a mocked sysfs/devmem.

## gles2tri.c

The actual question: does this board do 3D? Brings up EGL on the framebuffer,
compiles a shader pair, draws a triangle, and reads the result back out of the
GPU. Every stage is reported separately because they fail independently:

| stage | what it proves |
|---|---|
| `eglInitialize` | libIMGegl + libsrv_um reached the kernel driver |
| `eglCreateWindowSurface` | the WSEGL backend accepted /dev/fb0 |
| `eglCreateContext` | services connected, microkernel accepted |
| `glCompileShader` | libglslcompiler + libusc -- the USSE shader compiler |
| `glDrawArrays` + `glReadPixels` | the SGX actually rasterised something |

`GL_RENDERER` is the headline. If it names a PowerVR SGX and the centre pixel
comes back the green the fragment shader writes -- not the clear colour --
the GPU rendered it.

Two things are needed to build it against Intel's blobs:

* `-DMESA_EGL_NO_X11_HEADERS`. The DDK's `eglplatform.h` defaults to X11 and
  pulls in `Xlib.h`; that macro switches `EGLNativeWindowType` to a plain
  integer, which is what the fbdev backend wants (it takes no window handle).
* `libIMGegl.so` dlopens **`libpvrPVR2D_DRIWSEGL.so`** by that exact name, so
  to render to the framebuffer instead of X11, install
  `libpvrPVR2D_LINUXFBWSEGL.so` under that name. All five WSEGL backends
  export the same `WSEGL_GetFunctionTablePointer`, so they are drop-in.

```sh
i686-linux-gnu-gcc -O1 -DMESA_EGL_NO_X11_HEADERS -o gles2tri tools/ddk/gles2tri.c \
    -I<ddk>/usr/include -L<payload>/lib -lEGL -lGLESv2
```

