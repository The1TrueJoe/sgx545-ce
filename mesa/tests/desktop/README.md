# The desktop test

An Xfce desktop on a CE5300 box's monitor, on the fully open stack: Xorg's modesetting
driver with glamor on our Mesa (`sgx`), powervr_sgx and the open microkernel. Debian 13 (trixie)
userspace, as a Cedarview owner would run it.

| file | runs on | what |
|---|---|---|
| `packages.txt` | CI | the Debian packages in the root |
| `.github/workflows/desktop-root.yml` | CI | builds the root with mmdebstrap, maintainer scripts and all, with a user `desk`; the artifact `desktop-root-trixie` |
| `desktop-up.sh ROOT start\|stop\|run CMD` | the box | Xorg (`xorg.conf`) and an Xfce session as `desk`, on our Mesa in `ROOT/opt/sgx` |
| `xorg.conf` | the box | modesetting on the boot framebuffer with glamor; input through libinput |

## A run

1. Hold the box's lock. RAM-boot a powervr_sgx kernel with the open microkernel
   (`mesa/tests/x11/ce5300-ramboot.sh KERNDIR`). Stop anything else that owns the
   display.
2. Unpack the root on the box: `zstd -dc desktop-root-trixie.tar.zst | tar -xf - -C /opt/desk`.
   On a busybox box, unpack it on another machine and send it as a plain tar.
3. Put a Mesa `x86_64-x11` build in `/opt/desk/opt/sgx`: `lib/` and `share/`, with
   `share/glvnd/egl_vendor.d/50_mesa.json` naming `/opt/sgx/lib/libEGL_mesa.so.0`.
4. `desktop-up.sh /opt/desk start`. Use the desktop. `desktop-up.sh /opt/desk stop`.

"Usable" means windows move, the terminal and a browser draw, and a 30-minute session has no
GPU hang. Logs: `ROOT/var/log/Xorg.0.log`, `xorg.out`, `xfce.out`, and the kernel log.
