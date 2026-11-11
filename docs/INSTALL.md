# Installing the open PowerVR SGX545 driver on Debian

This guide installs the open graphics driver for the PowerVR SGX545 GPU on Debian 13
("trixie"). It covers:

- **Intel Cedarview** computers: Atom N2600, N2800, D2500, D2550 and D2700, with "Intel GMA
  3600" or "GMA 3650" graphics. Many 2012-era netbooks and small desktop boards.
- **Intel Atom CE5300 / CE5310** boards.

Everything here is open source. No Intel or Imagination software is installed.

> **Status: early.** 3D works in our tests, but long desktop sessions haven't been fully
> tested yet. Keep the rollback steps at the end of this page handy.

## What you install

There are three pieces, and each one is easy to remove again:

1. **A Linux kernel** with the GPU driver (`powervr_sgx`), as a Debian package. It installs
   next to Debian's own kernel, which stays on your computer and in the boot menu.
2. **The GPU firmware**, as the package `sgx545-firmware`.
3. **Mesa with the `sgx` driver** (OpenGL, OpenGL ES, EGL, GBM), as the package `mesa-sgx`,
   in `/opt/sgx`. Debian's own Mesa stays installed; ours is used first.

You need Debian 13 already installed, an internet connection, and a user that can run
`sudo`. The packages come from our apt repository, or as `.deb` files from a release on the
project's [Releases page](https://github.com/The1TrueJoe/powervr_sgx/releases).

## 1. Check what you have

Open a terminal and run:

```bash
lspci -nn | grep -i -E 'vga|display'
```

- `[8086:0be1]` or another `[8086:0be?]` on a "GMA 3600" line: **Cedarview**. Follow the
  Cedarview steps below.
- `[8086:089b]`: a **CE5300** GPU. Follow the CE5300 steps below.

## The apt repository

On Cedarview you can install all three pieces from our apt repository, and `apt upgrade`
then keeps them up to date. Add the repository's signing key (if `curl` is missing, install
it first with `sudo apt install curl`):

```bash
sudo curl -fsSLo /usr/share/keyrings/powervr-sgx.gpg https://telaak.dev/powervr_sgx/powervr-sgx.gpg
```

Check the key before you trust it:

```bash
gpg --show-keys /usr/share/keyrings/powervr-sgx.gpg
```

The long line under `pub` must read `5C26DC64CAEC63910424E220E0DD2B7AD794C167`. If it doesn't,
delete the file and stop. Then add the repository's entry:

```bash
printf 'Types: deb\nURIs: https://telaak.dev/powervr_sgx\nSuites: trixie\nComponents: main\nSigned-By: /usr/share/keyrings/powervr-sgx.gpg\n' | sudo tee /etc/apt/sources.list.d/powervr-sgx.sources
```

Then install the three packages:

```bash
sudo apt update
```

```bash
sudo apt install linux-image-7.3.3-sgx sgx545-firmware mesa-sgx
```

Restart, and go on with the checks in "3. The firmware" and "4. Mesa" below. To remove
the repository again, delete `/etc/apt/sources.list.d/powervr-sgx.sources` and
`/usr/share/keyrings/powervr-sgx.gpg`.

On a CE5300 board, add the repository the same way but install only
`sudo apt install sgx545-firmware mesa-sgx`, and build the kernel driver as in "2. The
kernel" below.

If you'd rather install the files by hand, download them from the newest release on the
[Releases page](https://github.com/The1TrueJoe/powervr_sgx/releases): the four `.deb` files
and `SHA256SUMS`. Put them in one folder and check them there:

```bash
sha256sum -c SHA256SUMS
```

Every line must end in `OK`. Sections 2 to 4 then install them one by one.

## 2. The kernel

The driver needs Linux 7.1 or newer. Debian 13 ships 6.12, so install our kernel package
(Linux 7.3.3).

### Cedarview

On Cedarview, the GPU shares its PCI device with the display driver (`gma500`). Our kernel
has a small change to `gma500` that hands the GPU to `powervr_sgx`. It is Debian's own
kernel configuration with that change added.

1. From the release, take `linux-image-7.3.3-sgx_7.3.3-sgx….deb` (the headers package
   next to it is only needed to build other kernel modules).
2. Install it:

   ```bash
   sudo apt install ./linux-image-7.3.3-sgx_*.deb
   ```

3. Restart. The new kernel is the default in the boot menu. Debian's kernel stays under
   "Advanced options".

### CE5300

There is no kernel package for CE5300 boards, because their kernels need board-specific
changes. Use a Linux 7.1 or newer kernel for your board, and build only the driver with
DKMS. The kernel must have four helper parts the driver uses. Check with:

```bash
grep -E 'DRM_(GPUVM|EXEC|SCHED|GEM_SHMEM_HELPER)=' /boot/config-$(uname -r)
```

All four must show `=y` or `=m`. Then, from a copy of this repository:

```bash
sudo apt install dkms build-essential linux-headers-$(uname -r)
sudo drm/dkms/mkdkms.sh 0.1
sudo dkms add powervr_sgx/0.1 && sudo dkms install powervr_sgx/0.1
```

If the old vendor driver `pvrsrvkm` is installed, block it so it doesn't take the GPU:

```bash
echo 'blacklist pvrsrvkm' | sudo tee /etc/modprobe.d/sgx-no-pvrsrvkm.conf
```

## 3. The firmware

From the release, take `sgx545-firmware_….deb` and install it:

```bash
sudo apt install ./sgx545-firmware_*.deb
```

It puts the file `/lib/firmware/powervr_sgx/sgx545_ukernel.bin` in place. Restart, or load
the driver now with `sudo modprobe powervr_sgx`.

### Check it

```bash
sudo dmesg | grep powervr_sgx
ls /dev/dri/
```

You should see lines like these, and a `renderD128` device:

```
powervr_sgx ...: SGX core id 0x01150000 revision 0x0001000d: Intel Cedarview ...
powervr_sgx ...: powervr_sgx/sgx545_ukernel.bin: open microkernel ..., features 0x2
powervr_sgx ...: microkernel started
```

On a CE5300 board, the first line reads `revision 0x0001000e: Intel CE5300 (SGX545 1.0.14)`
instead.

On Cedarview, `ls /sys/bus/auxiliary/devices/` should also list `gma500_gfx.sgx.` followed by a number. That
shows the kernel has the change that hands the GPU over.

If it says `no microkernel`, the firmware file isn't in the right place. If it says
`is not one this driver knows`, please report the two numbers on that line to us.

## 4. Mesa

From the release, take `mesa-sgx_….deb` and install it:

```bash
sudo apt install ./mesa-sgx_*.deb
```

It installs into `/opt/sgx`, with two small settings files that tell programs to use it
before Debian's Mesa. Log out and back in, or restart.

### Check it

```bash
sudo apt install mesa-utils
glxinfo -B | grep -E 'renderer|version'
```

The renderer line should name the **SGX545**. If it says `llvmpipe`, Debian's software
Mesa is still in use: check that steps 2 to 4 worked.

If the desktop reacts slowly while a game runs, let programs keep fewer frames in flight:
add the line `SGX_SLOTS=3` to `/etc/environment` and log in again. Games then run a little
slower. Delete the line to go back to the default of 4.

## Removing it again

You can undo each piece on its own.

- **Mesa:** `sudo apt remove mesa-sgx`. Programs use Debian's Mesa again.
- **The firmware:** `sudo apt remove sgx545-firmware`. On its own, it does nothing anyway.
- **The kernel (Cedarview):** pick Debian's kernel under "Advanced options" in the boot menu.
  To uninstall ours, run `sudo apt remove linux-image-7.3.3-sgx`.
- **DKMS (CE5300):** run `sudo dkms remove powervr_sgx/0.1 --all`. If you blocked
  `pvrsrvkm` and want it back, delete `/etc/modprobe.d/sgx-no-pvrsrvkm.conf`.

## Reporting problems

Please include:

- your computer model;
- the output of `sudo dmesg | grep -i -E 'powervr|gma500|drm'`;
- the output of `glxinfo -B`;
- what you were doing when it went wrong.

If the screen froze, the kernel log from that session helps us most. After the restart,
run:

```bash
sudo journalctl -k -b -1 | grep -i -E 'powervr|gma500|drm'
```

That shows the previous boot's kernel messages. It only works if the journal is kept on
disk, so turn that on once with `sudo mkdir -p /var/log/journal` and a restart. If you can
reach the computer over the network while it is frozen, run `sudo dmesg` there instead.
