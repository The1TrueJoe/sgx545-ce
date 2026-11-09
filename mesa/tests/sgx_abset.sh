#!/bin/sh
# The whole A/B set against the DDK on the EA-1, under one GPU lock, stopping
# at the first SGX recovery (the box has no plug). Run on the box from /tmp/mesa,
# with the panel free (mc-session and the web view stopped).
# Prints each suite's lines, then the totals.
cd /tmp/mesa
export GPU_LOCK_HELD=1
exec 9>/tmp/gpu.lock
flock 9
R0=$(dmesg | grep -c 'Recovery triggered')
for s in varyab texab texproj texshape uniforms nearclip attrs drvconsts blendab discardab; do
	echo "== $s"
	sh ./sgx_$s.sh 2>&1
	R=$(dmesg | grep -c 'Recovery triggered')
	if [ "$R" != "$R0" ]; then
		echo "abset: SGX recovery in $s: stopping"
		exit 1
	fi
done
