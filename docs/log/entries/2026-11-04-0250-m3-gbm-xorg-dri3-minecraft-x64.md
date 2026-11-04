# GBM, Xorg with DRI3, and Minecraft on a 64-bit JVM, all on powervr_sgx

The display path after M3. EA1 RAM boot of m2-drm c95e32d (CI run 41752797996) with the DDK
microkernel, 2026-11-04 02:52 to 03:30 UTC. Nothing was flashed and there was no power-cycle.
The box is back on its normal boot.

## 1. GBM and kmsro on simpledrm

The test kernel drives the CEFDK framebuffer with simpledrm (card0, 720x480).
`mesa/patches/0006` lets Mesa's kmsro pair that KMS-only device with powervr_sgx's render
node (card1, `renderD128`).

`mesa/tests/sgx545_kmscube.c` does the following:
- renders GLES2 frames through GBM on card0 and flips them onto the display;
- clears each frame to its own colour and draws a green triangle;
- checks the last frame in the memory the display scans out, read through `/dev/mem`.

| scanout buffer | FPS | scanout check |
|---|---|---|
| ours, exported to simpledrm | 3.6 | right |
| simpledrm's dumb buffer, imported | 100-108 | right, 6 of 6 runs |

simpledrm copies every frame out of its framebuffer with the CPU. Reading our write-combined
objects that way is slow. A dumb buffer has cached pages, and since c95e32d powervr_sgx
flushes the CPU caches of an imported buffer around every job that lists it (log 0100). The
winsys always lists the frame's target. So kmsro now imports the display's dumb buffer, and
the driver keeps the export path for other display devices.

## 2. Xorg, glamor, DRI3

Xorg 21.1 (Debian bookworm) runs in a Debian amd64 root: `mesa/tests/x11/mkroot.py` builds
it from the package index alone, without debootstrap and without Debian's own Mesa. The
setup:
- the modesetting driver on simpledrm with glamor on our driver, through GBM and kmsro;
- GLX through libglvnd, the `x86_64-x11` build in `mesa/build.sh`;
- AIGLX through dril stubs for simpledrm and sgx545 (patch 0006);
- `mesa/tests/x11/x11-up.sh` starts it and runs clients in the root.

What it took:
- **Flushes into shared buffers.** The driver defers a plain `glFlush` of an off-screen
  frame. glamor's screen pixmap is a shared dma-buf, and its frames sat unsubmitted, so
  nothing reached the display. A flush into a shared buffer is now never deferred.
- **EGLImages of dma-bufs as textures.** glamor samples the screen pixmap through an
  EGLImage. The four 32-bit render-target formats are now sampler views; they are sampled
  in place through the strided layout.

Results:
- `glxinfo -B`: direct rendering, `Using DRI3 for screen 0`, renderer `PowerVR SGX545
  (sgx545-ce)`, **OpenGL 2.0** (GLSL 1.20), OpenGL ES 2.0.
- `glxgears`: 331-339 FPS in its default window; 143 FPS at 300x300 with Xorg compositing it
  onto the screen.
- No job timeouts.
- The capture `img/2026-11-04-0315-xorg-dri3-glxgears-scanout.png` is read back from the
  scanout memory.

**Not done: GL 2.1, so a shim stays for now.** glamor needs desktop GL 2.1, or else its GLES
path, and the GLES path stops on `glTexImage2D(GL_R8)`. Xorg itself (not the clients) runs
with `MESA_GL_VERSION_OVERRIDE=2.1 MESA_GLSL_VERSION_OVERRIDE=120` until the driver reaches
2.1. That is M6, likely only `EXT_texture_sRGB` short.

## 3. Minecraft 1.7.10 on a 64-bit JVM

`mesa/tests/x11/mc-x64.sh` starts vanilla 1.7.10, offline:
- Zulu 8 x64 JRE (sha256 checked against Azul's metadata);
- LWJGL 2.9.1's amd64 natives (`liblwjgl64.so`) from the openhc-minecraft install;
- the game files copied from that install into the root's `/opt/mc-x64`; the install and the
  user's worlds were not touched.

LWJGL opens a GLX window on the X server above. Its libGL is libglvnd's, which dispatches to
our Mesa. There is no gl4es and no override in the game's environment. Sound is off, because
the chroot has no ALSA device.

Results:
- The title screen renders: `img/2026-11-04-0320-minecraft-x64-glx-title.png`. Its panorama
  is drawn only in its lower-left quarter; the rest is grey, not yet looked into.
- A new world loads and renders: textured terrain, water and the HUD
  (`img/2026-11-04-0325-minecraft-x64-glx-world.png`). The player spawned in water and
  drowned, since there is no input but `xdotool`.
- No job timeouts.

## Left

- GL 2.1 in the driver (M6), then Xorg without the override.
- The title panorama.
- piglit's GLX tests, which need a piglit build against the X11 root.
- A frame-rate measurement in a world.
- On the EA1's p1: `/opt/x64root` (354 MB) and `/root/m3`, `/root/deqp/lib-m3*`, all files
  only.
