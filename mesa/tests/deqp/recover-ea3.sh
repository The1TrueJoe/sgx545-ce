#!/bin/sh
# Power-cycle the EA3 through the Bench plug and wait for it. The kernel step is
# RECOVERY_BOOT, if set: run on this machine once the box answers, it boots
# the kernel to test from RAM before anything touches the GPU.
BENCH=${BENCH:-$HOME/.cache/sgx545/bench.py}
BOX=${EA3:-10.0.0.113}
python3 "$BENCH" cycle
i=0
until sshpass -p openhc ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$BOX true 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 60 ] && { echo "recover-ea3: the box didn't come back"; exit 1; }
	sleep 5
done
if [ -n "$RECOVERY_BOOT" ]; then
	sh -c "$RECOVERY_BOOT" || exit 1
fi
