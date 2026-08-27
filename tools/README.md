# tools

## sgxregs.sh
Reads the SGX identity registers on the board. Run this before anything else —
it answers "is the GPU there, clocked, and where we think it is" in a minute.
`test-sgxregs.sh` self-checks its arithmetic against a mocked sysfs/devmem.

## sgxinit.c
Brings up PVR services against the kernel driver: the userspace half of the
bring-up, and the thing that first exercises `open()` on the DRM node.

It exists because neither available userspace can be used as-is:

* Intel's Cedarview package ships **no** `pvrsrvctl` — libraries only.
* Control4's `pvrsrvctl` is DDK 1.12 and links 1.12's `libsrv_um`/`libsrv_init`,
  so it cannot drive a 1.7 userspace.

But all `pvrsrvctl --start` really does is one call. Disassembling Control4's
binary shows `SrvInit` takes no arguments and returns a `PVRSRV_ERROR` in `eax`
(nothing is pushed before the call, and a non-zero return goes straight to
`PVRSRVGetErrorString`). So this is that call, and nothing else. It `dlopen`s
rather than links, so an unresolved symbol prints instead of the loader
refusing to start the process.

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
i686-linux-gnu-gcc -O1 -DMESA_EGL_NO_X11_HEADERS -o gles2tri tools/gles2tri.c \
    -I<ddk>/usr/include -L<payload>/lib -lEGL -lGLESv2
```

## busidshim.c

Needed to run Intel's Cedarview DDK 1.7 userspace on a CE5300.

`libsrv_um.so` calls `drmOpen(NULL, "pci:0000:00:02.0")` with that bus id
hardcoded -- it is where the SGX sits on Cedarview. On the CE5300 the SGX is
its own PCI function one bus further out, at `0000:01:02.0`, so
`drmOpenByBusid()` matches nothing, returns -1, and `SrvInit()` fails with
`PVRSRV_ERROR_INIT_FAILURE` having never issued a single ioctl -- which is why
the kernel log is completely empty when this happens.

The blob cannot be edited (Intel's licence forbids it), so `LD_PRELOAD` this
and it corrects the bus id on the way through. `SGX_BUSID` overrides the
replacement so it is not pinned to one board.

```sh
i686-linux-gnu-gcc -shared -fPIC -o busidshim.so tools/busidshim.c -ldl
LD_PRELOAD=./busidshim.so ./sgxinit
```

Control4's DDK 1.12 blobs do not need this -- they call
`drmOpen("pvrsrvkm", NULL)` and match on driver name instead -- but they fail
the DDK version check against this 1.7 tree.

### Running it

The DDK blobs are 32-bit glibc; openHC is musl. Rather than fight that during
bring-up, run them against a private glibc sysroot:

```sh
# on a build host: assemble lib/ from Debian i386 libc6, libdrm2, libgcc-s1
# plus the DDK's libsrv_um.so and libsrv_init.so, then
i686-linux-gnu-gcc -O0 -g -o sgxinit tools/sgxinit.c -ldl

# on the board
./lib/ld-linux.so.2 --library-path /opt/pvrtest/lib ./bin/sgxinit
```

Pair the blobs with a driver built from the **same DDK version** — the version
check is exact-match. Cedarview's 1.7 blobs match this tree; Control4's 1.12
blobs will be rejected.

Watch `dmesg` alongside it. The first run of this found the driver's missing
`FOP_UNSIGNED_OFFSET`, which no amount of building would have surfaced.
