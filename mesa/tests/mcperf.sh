#!/bin/sh
# EA1: profile WHO in the bench world: perf on the game's java for 20 s, ~85 s after the menu
WHO=${1:-sgx545}
R=/opt/minecraft/rootfs
sh /tmp/mesa/mcbench.sh $WHO 140 > /tmp/mesa/mcperf-bench.txt 2>&1 &
b=$!
L=$R/ctest/out/mc-$WHO.log
n=0; while ! grep -q foliage.png $L 2>/dev/null && [ $n -lt 300 ]; do sleep 1; n=$((n+1)); done
sleep 85
pid=$(pidof java)
tid=$(for t in /proc/$pid/task/*; do [ "$(cat $t/comm)" = "Client thread" ] && basename $t; done | head -1)
echo "client thread $tid"
chroot $R /usr/bin/perf record -F 4000 -t $tid -o /ctest/perf-$WHO.data -- sleep 20 > /tmp/mesa/perf-rec.txt 2>&1
cat /tmp/mesa/perf-rec.txt | tail -2
wait $b
grep "FPS" /tmp/mesa/mcperf-bench.txt | tail -3
P="chroot $R /usr/bin/perf report -i /ctest/perf-$WHO.data --no-children --stdio"
$P --sort dso 2>/dev/null | grep -v "^#" | grep -v "^$" | head -14
$P --sort dso,sym 2>/dev/null | grep -v "^#" | grep -v "^$" | head -70
