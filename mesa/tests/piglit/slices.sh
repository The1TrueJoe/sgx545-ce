#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# piglit's gpu profile on a shared EA in slices: drive-deqp.py piglit holds
# /tmp/gpu.lock for at most a slice, then this waits so other sessions get a
# turn. A slice starts only on the box's normal boot with no SGX recoveries
# that aren't ours (drive-deqp.py's box check); results carry over between
# slices and reboots.
#
#   mesa/tests/piglit/slices.sh NAME [SLICE_MINUTES]
#
# MESA_DIR is the x86_64-gl Mesa to test (copied to the box as lib-NAME);
# BOX the board (the GPU board), BOOT=ramboot for a board whose slices run on a RAM boot
# the runner marked (drive-deqp.py --boot; the board);
# results go to $PIGLIT_WORK/run-NAME. drive-deqp.py exits 0 when every test
# has a result, 3 at a slice's end and 4 when the box is someone else's; the
# loop stops for good on anything else (a wedge, or too many recoveries).
# To hand the box over mid-slice: kill this loop, then the drive-deqp.py it runs
# (SIGTERM: it lets the lock go on its way out).
HERE=$(cd "$(dirname "$0")" && pwd)
W=${PIGLIT_WORK:-$HOME/.cache/sgx545/piglit}
NAME=${1:?name}
SLICE=${2:-30}
BOX=${BOX:-board}
BOOT=${BOOT:-normal}
PY=${PYTHON:-python3}
while :; do
	RECOVER=none DEQP_WORK="$W" "$PY" "$HERE/../deqp/drive-deqp.py" piglit --box "$BOX" --boot "$BOOT" --name "$NAME" \
		--chunk 50 --hold-lock --max-recoveries 3 --slice-minutes "$SLICE" >> "$W/run-$NAME.log" 2>&1
	rc=$?
	case $rc in
	0) echo "$(date +%T) all done" >> "$W/run-$NAME.log"; exit 0 ;;
	3) sleep 600 ;;     # a turn for the others
	4) sleep 300 ;;
	*) echo "$(date +%T) stopped: rc=$rc" >> "$W/run-$NAME.log"; exit $rc ;;
	esac
done
