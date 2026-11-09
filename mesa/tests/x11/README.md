# X11, GLX and Minecraft on powervr_sgx

An X server and GLX clients on the EA, all on our Mesa driver through powervr_sgx, with no
gl4es and no shims in the clients. Logs: `docs/log/entries/2026-11-04-0250-*` and later.

| file | runs on | what |
|---|---|---|
| `mkroot.py` | the Mac | a Debian bookworm amd64 root from the package index (no debootstrap, no Debian Mesa): Xorg, xkb, glxinfo/glxgears, the X11 and glvnd development files |
| `xorg.conf` | the EA, in the root | modesetting on simpledrm (card0) with glamor; no input devices |
| `x11-up.sh ROOT start\|stop\|run CMD` | the EA | Xorg in the root (its dev, proc, sys and tmp bound in) and clients against `:0` on our Mesa |
| `mc-x64-stage.sh ROOT` | the EA | copies Minecraft from the box's Minecraft install into `ROOT/opt/mc-x64`, read-only on the install's side |
| `mc-x64.sh` | the EA, in the root | Minecraft 1.7.10 on a 64-bit JVM, as the install's `minecraft-ab` |
| `ce5300-ramboot.sh KERNDIR [ddk\|open]` | the host | RAM-boots an m2-drm `powervr-sgx-ea1-v1` artifact on the EA1 and loads powervr_sgx with the named microkernel |
| `memdump.c` | the EA | physical memory through `/dev/mem`: the scanout at 0x7fc00000, stride 2944 |
| `mc-x64-bench.sh ROOT [SECS]` | the EA | `mcbench.sh`'s benchmark world and clicks on this path, FPS and job timeouts |

## From nothing to a run

1. **Build**, on the Mac:
   - `python3 -I mesa/tests/x11/mkroot.py ~/Documents/GitHub/sgx545-work/<session>/build/x64/root <packages>`, with the
     packages listed in the 0250 log entry, plus `xdotool` for the benchmark's clicks.
   - `X11_SYSROOT=<root> mesa/build.sh x86_64-x11`.
   - Put the build's `lib` and `share` in `<root>/opt/sgx`. Point
     `share/glvnd/egl_vendor.d/50_mesa.json` at `/opt/sgx/lib/libEGL_mesa.so.0`.
   - Put a Zulu 8 x64 JRE in `<root>/opt/java/jre8-x64`; check its sha256 against Azul's
     metadata API.
2. **Boot**: hold `~/Documents/GitHub/sgx545-work/locks/ea1.lock`. RAM-boot an m2-drm `powervr-sgx-ea1-v1`
   artifact with the one-shot `drm/ci/ea-ramboot/S00aramboot`. Load its modules, and load
   `powervr_sgx` with `firmware=powervr_sgx/sgx545-ukernel-1.7.862890.bin` unless the run
   is meant to use the open microkernel: the driver prefers `sgx545_ukernel.bin`
   whenever it's present.
3. **Copy the root** to the box with `gunzip -c | tar -xof -` into `/opt/x64root`. Busybox
   tar has no `-z`, and `-o` keeps root ownership. Never remove `/opt/x64root` while
   anything is mounted under it: x11-up.sh binds the box's own `/dev` and `/tmp` there, and
   an `rm -rf` through them empties both. `grep ' /opt/x64root/' /proc/mounts` must print
   nothing first.
4. **Stage the game**: `mc-x64-stage.sh /opt/x64root`.
5. **Run**:
   - `x11-up.sh /opt/x64root start`;
   - `x11-up.sh /opt/x64root run glxinfo -B`, or `... run /opt/mc-x64/mc-x64.sh`, or
     `mc-x64-bench.sh /opt/x64root 170`;
   - `x11-up.sh /opt/x64root stop`.
6. **Restore**: `rm -rf /boot/once` and a plain `reboot`. Check that pvrsrvkm is bound
   again, then release `ea1.lock`.

## What isn't right yet

- **Xorg runs with `MESA_GL_VERSION_OVERRIDE=2.1`**, in `x11-up.sh`'s environment for the
  server. glamor's desktop path wants GL 2.1. Ours is 2.0, short only of `EXT_texture_sRGB`.
  Clients run without it (`env -u`).
- **The title panorama** draws only its lower-left quarter (mesa-gallium has it).
- **No sound**: the root has no ALSA device.
