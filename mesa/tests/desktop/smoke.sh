#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# From the host: the Cedarview stick's desktop smoke test (kit/cedarview-live/
# kit-smoke) on a CE5300 box, on the packages a user installs. Take the box's
# lock and RAM-boot a powervr_sgx kernel first (ce5300-ramboot.sh).
#
#   BOX=host BOX_PASS=... smoke.sh ROOT.tar.zst MESA_SGX.deb OUTDIR
#
# The root (desktop-root.yml) is unpacked once to $DESK on the box; mesa-sgx is
# installed in it with dpkg. Results, screenshots, the kernel log, the X and Xfce
# logs and any devcoredump land in OUTDIR. XENV is passed to Xorg (desktop-up.sh).
set -eu
TAR=$1 DEB=$2 OUT=$3
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../../.." && pwd)
KIT=$REPO/kit/cedarview-live
BOX=${BOX:?the address of the box}
DESK=${DESK:-/opt/desk}
E() { sshpass -p "${BOX_PASS:?the root password of the box}" ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o ServerAliveInterval=15 root@$BOX "$@"; }
mkdir -p "$OUT"
tag=$(shasum -a 256 "$TAR" | cut -c1-16)
if [ "$(E "cat $DESK/.root-tag 2>/dev/null" || true)" != "$tag" ]; then
	E "grep -q ' $DESK/' /proc/mounts && { echo 'binds still mounted in $DESK' >&2; exit 1; }; \
		rm -rf ${DESK:?}; mkdir -p $DESK"
	zstd -dc "$TAR" | E "tar -x -C $DESK && echo $tag > $DESK/.root-tag"
fi
E "mkdir -p $DESK/root/rig $DESK/usr/local/share/kit"
for f in desktop-up.sh xorg.conf; do E "cat > $DESK/root/rig/$f" < "$here/$f"; done
E "cat > $DESK/usr/local/bin/kit-smoke && chmod 755 $DESK/usr/local/bin/kit-smoke" < "$KIT/kit-smoke"
E "cat > $DESK/usr/local/share/kit/test.html" < "$KIT/test.html"
E "cat > $DESK/tmp/mesa-sgx.deb" < "$DEB"
E "cd $DESK && for d in dev proc sys; do mount --bind /\$d \$d; done; \
	chroot . dpkg -i /tmp/mesa-sgx.deb; r=\$?; umount sys proc dev; rm -f tmp/mesa-sgx.deb; exit \$r" \
	> "$OUT/dpkg.txt" 2>&1 || { cat "$OUT/dpkg.txt"; exit 1; }
E "rm -rf /tmp/results; mkdir -p -m 1777 /tmp/results /tmp/results/screenshots; \
	ls /sys/class/devcoredump/ 2>/dev/null | wc -l"
E "XENV='${XENV:-}' sh $DESK/root/rig/desktop-up.sh $DESK start" > "$OUT/start.txt" 2>&1 || {
	cat "$OUT/start.txt"; E dmesg > "$OUT/dmesg.txt"; exit 1; }
t0=$(date +%s)
E "sh $DESK/root/rig/desktop-up.sh $DESK run /usr/local/bin/kit-smoke /tmp/results" > "$OUT/smoke-run.txt" 2>&1 || true
echo "smoke test: $(( $(date +%s) - t0 )) s" | tee "$OUT/time.txt"
E "dmesg" > "$OUT/dmesg.txt" 2>&1
E "for d in /sys/class/devcoredump/devcd*; do [ -e \$d/data ] && cat \$d/data > /tmp/results/\$(basename \$d).bin; done; true"
E "sh $DESK/root/rig/desktop-up.sh $DESK stop" > "$OUT/stop.txt" 2>&1 || true
E "cp $DESK/var/log/Xorg.0.log $DESK/var/log/xorg.out $DESK/var/log/xfce.out /tmp/results/ 2>/dev/null; \
	cd /tmp && tar -cf - results" | tar -xf - -C "$OUT"
echo "== $OUT/results/smoke.txt"
cat "$OUT/results/smoke.txt" 2>/dev/null || echo "(none)"
grep -E 'powervr_sgx.*(fault|lost|hang|recover|reset)' "$OUT/dmesg.txt" | head -10 || true
