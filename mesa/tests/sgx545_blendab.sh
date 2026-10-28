#!/bin/sh
# blend A/B on the EA3: the DDK (i386 probe-st) against ours (x86_64 build),
# the probe's reference scene drawn with a half-transparent colour over the clear
cd /tmp/mesa
export PROBE_COLOR="0.25 1 0.5 0.5"
run() {
	name=$1; shift
	env "$@" PROBE_OUT=/tmp/mesa/bab-ddk-$name.rgba ./gpurun.sh /opt/sgx-i386/run.sh probe-st > /tmp/mesa/bab-ddk-$name.log 2>&1
	env "$@" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/bab-ours-$name.rgba ./gpurun.sh ./probe-x86_64 > /tmp/mesa/bab-$name.log 2>&1
	a=$(md5sum < bab-ddk-$name.rgba); b=$(md5sum < bab-ours-$name.rgba)
	[ "$a" = "$b" ] && r=IDENTICAL || r=DIFFER
	echo "$name: $r ddk $(grep -o 'centre rgba([0-9,]*)' bab-ddk-$name.log) ours $(grep -o 'centre rgba([0-9,]*)' bab-$name.log) $(grep -i -c 'recovery' bab-$name.log)"
}
# GL enums: 0 ZERO, 1 ONE, 768 SRC_COLOR, 769 1-SRC_COLOR, 770 SRC_ALPHA,
# 771 1-SRC_ALPHA, 772 DST_ALPHA, 773 1-DST_ALPHA, 774 DST_COLOR,
# 775 1-DST_COLOR, 776 SRC_ALPHA_SATURATE; 32774 ADD, 32775 MIN, 32776 MAX,
# 32778 SUBTRACT, 32779 REVERSE_SUBTRACT
run alpha PROBE_BLEND="770 771"
run add PROBE_BLEND="1 1"
run premul PROBE_BLEND="1 771"
run mul PROBE_BLEND="774 0"
run srccol PROBE_BLEND="768 769"
run invdst PROBE_BLEND="775 1"
run dstalpha PROBE_BLEND="772 773"
run sat PROBE_BLEND="776 1"
run sub PROBE_BLEND="1 1 32778"
run revsub PROBE_BLEND="770 1 32779"
run min PROBE_BLEND="1 1 32775"
run max PROBE_BLEND="1 1 32776"
