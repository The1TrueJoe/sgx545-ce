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
`sudo`. Download everything from the CI run linked in the announcement. You need to be
signed in to GitHub to download. Each download is a `.zip`; unzip it first.

## 1. Check what you have

Open a terminal and run:

```bash
lspci -nn | grep -i -E 'vga|display'
```

- `[8086:0be1]` or another `[8086:0be?]` on a "GMA 3600" line: **Cedarview**. Follow the
  Cedarview steps below.
- `[8086:089b]`: a **CE5300** GPU. Follow the CE5300 steps below.

## 2. The kernel

The driver needs Linux 7.1 or newer. Debian 13 ships 6.12, so install our kernel package.

### Cedarview

On Cedarview, the GPU shares its PCI device with the display driver (`gma500`). Our kernel
has a small change to `gma500` that hands the GPU to `powervr_sgx`. It is Debian's own
kernel configuration with that change added.

1. From the CI download, take `linux-image-7.1.8-sgx_7.1.8-sgx1_amd64.deb`.
2. Install it:

   ```bash
   sudo apt install ./linux-image-7.1.8-sgx_*.deb
   ```

3. Restart. The new kernel is the default in the boot menu. Debian's kernel stays under
   "Advanced options".

### CE5300

There is no kernel package for CE5300 boards, because their kernels need board-specific
changes. Use a Linux 7.1 or newer kernel for your board, and build only the driver with
DKMS. From a copy of this repository:

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

From the CI download, take `sgx545-firmware_….deb` and install it:

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

On Cedarview, `ls /sys/bus/auxiliary/devices/` should also list `gma500_gfx.sgx.0`. That
shows the kernel has the change that hands the GPU over.

If it says `no microkernel`, the firmware file isn't in the right place. If it says
`is not one this driver knows`, please report the two numbers on that line to us.

## 4. Mesa

From the CI download, take `mesa-sgx_….deb` and install it:

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

## Removing it again

You can undo each piece on its own.

- **Mesa:** `sudo apt remove mesa-sgx`. Programs use Debian's Mesa again.
- **The firmware:** `sudo apt remove sgx545-firmware`. On its own, it does nothing anyway.
- **The kernel (Cedarview):** pick Debian's kernel under "Advanced options" in the boot menu.
  To uninstall ours, run `sudo apt remove linux-image-7.1.8-sgx`.
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
