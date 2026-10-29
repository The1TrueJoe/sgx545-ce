#!/bin/sh
# EA1: one run on WHO (sgx545|ddk) in the fixed benchmark world (/opt/mc-ab/bench-world,
# restored before every run), SECS after the menu; prints FPS + stats
WHO=${1:-sgx545} SECS=${2:-150}
R=/opt/minecraft/rootfs A=$R/opt/mc-ab
rm -rf $A/saves/*; cp -a $A/bench-world "$A/saves/bench"
rm -f $R/ctest/out/mc-$WHO*
env="MC_DIR=/opt/mc-ab"; [ $WHO = sgx545 ] && env="LIBGL_NOTEST=1 MC_DIR=/opt/mc-ab SGX545_STATS=1 $MC_EXTRA"
MC_READY="foliage.png" MC_BURST=${MC_BURST:-2} MC_ENV="$env" /tmp/mesa/mc-ab.sh $WHO $SECS "10:mousemove 360 236" "12:click 1" "30:mousemove 360 205" "32:mousedown 1" "33:mouseup 1" "40:mousemove 202 396" "42:click 1" 2>&1 | grep "mc-ab:\|gpuwatch"
L=$R/ctest/out/mc-$WHO.log
grep "LIBGL: fps" $L | sed -n '$!p' | tail -12 | grep -o "fps: [0-9.]*" | tr "\n" " "; echo
grep "sgx545 stats" $L | tail -4
grep "sgx545:" $L | sort | uniq -c | head
echo "recoveries $(dmesg | grep -c 'Recovery triggered') temp $(cat /sys/class/hwmon/hwmon*/temp1_input)"
