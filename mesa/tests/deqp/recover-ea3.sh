#!/bin/sh
# Power-cycle the EA3 through the Bench plug and boot the kernel under test
# from RAM (kexec is only safe before anything touches the GPU): the first boot
# comes up on p1's install, gets a one-shot init script that kexecs the image
# in /boot/once and removes itself, and a second cycle runs it.  A hang costs
# one power cycle back to the normal install.  Run with "install IMAGE_DIR" once
# to copy the image to p1, and "off" at the end of a slot to remove the script.
#   SGXINIT=ddk (default): the DDK's sgxinit; SGXINIT=: srvinit
BENCH=${BENCH:-$HOME/.cache/sgx545/bench.py}
BOX=${EA3:-10.0.0.113}
SGXINIT=${SGXINIT-ddk}
SSH="sshpass -p openhc ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$BOX"

wait_box() {
	i=0
	until $SSH "$1" 2>/dev/null; do
		i=$((i + 1))
		[ $i -gt 60 ] && { echo "recover-ea3: no answer from the box ($1)"; exit 1; }
		sleep 5
	done
}

case "$1" in
install)
	for f in bzImage-real:bzImage rootfs.cpio.gz:rootfs.cpio.gz; do
		$SSH "mkdir -p /boot/once && cat > /boot/once/${f#*:}" < "$2/${f%:*}" || exit 1
	done
	$SSH "sync; ls -la /boot/once; df -h /boot/once | tail -1"
	exit 0 ;;
off)
	$SSH "rm -f /etc/init.d/S00aramboot; sync"
	exit 0 ;;
esac

CMD="console=ttyS0,115200 pci=realloc,nocrs,routeirq intel_iommu=off root=/dev/mmcblk0p1 rootwait rw ohc_kexec=1 ohc.ramboot=1${SGXINIT:+ ohc.sgxinit=$SGXINIT}"
python3 "$BENCH" cycle
wait_box "test -f /boot/once/bzImage"
$SSH "cat > /etc/init.d/S00aramboot; chmod +x /etc/init.d/S00aramboot; sync" <<SCRIPT
#!/bin/sh
rm -f /etc/init.d/S00aramboot
sync
grep -q ohc_kexec= /proc/cmdline && exit 0
kexec -l /boot/once/bzImage --initrd=/boot/once/rootfs.cpio.gz --command-line="$CMD"
sync
kexec -e
SCRIPT
python3 "$BENCH" cycle
wait_box "grep -q ohc.ramboot=1 /proc/cmdline"
sleep 20
