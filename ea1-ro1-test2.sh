#!/bin/zsh
# EA1 render_done-on-0x1 HW test (uk-ro1.bin, commit 1cccc1d) -- v2, robust reboot
# detection. Waits for an actual NEW boot_id + expected cmdline (auth blips can't
# fool it), and greps the ro1 run tag (can't match a stale clean3 log). Same proven
# cold-GPU ramboot rig. ALWAYS restores to normal boot.
setopt sh_word_split
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
H=root@10.0.0.110
HW=/private/tmp/claude-501/-Users-josephtelaak-Documents-GitHub-openHC--claude-worktrees-optimistic-leakey-b1d663/ca698cd7-46e4-4e13-949b-9c4a8185b579/scratchpad/sgx/hwruns
BIN=uk-ro1.bin
TAG=ro1-render-on-0x1
ssh_() { sshpass -p openhc ssh $OPTS "$H" "$@" 2>&1 | grep -v "Permanently added"; }
# wait_boot <old_boot_id> <NORMAL|RAMBOOT>: poll until reachable + boot_id changed + cmdline matches
wait_boot() {
  old=$1; want=$2
  for i in $(seq 1 50); do
    sleep 4
    r=$(ssh_ 'echo "$(head -c11 /proc/sys/kernel/random/boot_id) $(grep -o ohc.ramboot /proc/cmdline || echo NORMAL)"' 2>/dev/null)
    bid=$(echo "$r" | awk '{print $1}'); cm=$(echo "$r" | awk '{print $2}')
    [ -z "$bid" ] && { printf "." ; continue; }
    want2=NORMAL; [ "$want" = RAMBOOT ] && want2=ohc.ramboot
    echo " [$i] boot_id=$bid cmdline=$cm want=$want2"
    [ "$cm" = "$want2" ] && [ -n "$bid" ] && [ "$bid" != "$old" ] && { echo "  -> $want OK"; return 0; }
  done
  echo "  -> $want NEVER REACHED"; return 1
}

echo "=== precheck: box must be on NORMAL boot, GPU cold ==="
pre=$(ssh_ 'echo "$(grep -o ohc.ramboot /proc/cmdline || echo NORMAL) pvr=$(lsmod|grep -ic pvr)"')
echo "precheck: $pre"
echo "$pre" | grep -q '^NORMAL' || { echo "FATAL: box not on normal boot ($pre) -- stabilize first"; exit 3; }

echo "=== on-box lock + stage $BIN ==="
ssh_ "echo ukernel-ea1-ro1 > /tmp/gpu.lock"
sshpass -p openhc ssh $OPTS "$H" "cat > /root/uk/uk-ro1.bin" < "$HW/uk-ro1.bin" 2>&1 | grep -v "Permanently added"
sshpass -p openhc ssh $OPTS "$H" "cat > /root/uk/uk-ro1.bin.pat" < "$HW/uk-ro1.bin.pat" 2>&1 | grep -v "Permanently added"
echo "staged md5 remote=$(ssh_ "md5sum /root/uk/$BIN | cut -c1-8") local=$(md5 -q "$HW/uk-ro1.bin" | cut -c1-8)"

echo "=== install S50 (ro1) + S00aramboot + /boot/once ==="
cat > /tmp/ea1-install2.sh <<INSTALL
#!/bin/sh
set -e
cat > /etc/init.d/S50zuktest <<'EOS'
#!/bin/sh
[ "\$1" = start ] || exit 0
case " \$(cat /proc/cmdline) " in *ohc.ramboot*) ;; *) exit 0 ;; esac
L=/root/uk/test.log; : > "\$L"
echo "=== S50 $TAG \$(date 2>/dev/null) ===" >> "\$L"
i=0; while [ \$i -lt 15 ] && [ ! -e /dev/dri/card0 ]; do sleep 1; i=\$((i+1)); done
cd /root/uk || exit 0
echo "--- srvinit -o $BIN ---" >> "\$L"
./srvinit -o /root/uk/$BIN >> "\$L" 2>&1
echo "--- 10 frames ---" >> "\$L"
r=0; t=0
for f in 1 2 3 4 5 6 7 8 9 10; do
  o=\$(./frame-dump -o /tmp/f\$f.bin 2>&1); echo "\$o" | tail -1 >> "\$L"
  echo "\$o" | grep -qi "render done" && r=\$((r+1)) || t=\$((t+1))
done
echo "--- rate + HWR + md5 ---" >> "\$L"
echo "RENDERED=\$r TIMEOUT=\$t" >> "\$L"
echo "HWR=\$(dmesg 2>/dev/null | grep -icE 'SGX lockup|Hardware Recovery')" >> "\$L"
md5sum /tmp/f1.bin >> "\$L" 2>/dev/null
echo "--- cleanup-timeout watch (host force-free?) ---" >> "\$L"
dmesg 2>/dev/null | grep -iE 'cleanup|CleanupStatus|force|freed|timed? ?out' | tail -15 >> "\$L"
echo "--- (P0) BIF / BANK0 ---" >> "\$L"
dmesg 2>/dev/null | grep -iE 'SGX lockup|BIF_BANK0|BIF_FAULT|EVENT_STATUS|lockup_ta|lockup_3d|dpm_|Hardware Recovery' | tail -40 >> "\$L"
echo "=== S50 $TAG complete ===" >> "\$L"
exit 0
EOS
chmod +x /etc/init.d/S50zuktest
cat > /etc/init.d/S00aramboot <<'EOS'
#!/bin/sh
[ "\$1" = start ] || exit 0
rm -f /etc/init.d/S00aramboot
sync; mount -t proc proc /proc 2>/dev/null; mount -t sysfs sys /sys 2>/dev/null
case " \$(cat /proc/cmdline 2>/dev/null) " in *" ohc_kexec="*) exit 0 ;; esac
K=/boot/once/bzImage; [ -f "\$K" ] || exit 0
command -v kexec >/dev/null 2>&1 || exit 0
[ -r /opt/ohc/board.env ] && . /opt/ohc/board.env
CMDLINE="console=ttyS0,115200 pci=realloc,nocrs,routeirq intel_iommu=off root=/dev/mmcblk0p1 rootwait rw ohc_kexec=1 ohc.ramboot=1 \${OHC_KEXEC_CMDLINE_EXTRA:-}"
if kexec -l "\$K" --command-line="\$CMDLINE"; then sync; kexec -e; fi
exit 0
EOS
chmod +x /etc/init.d/S00aramboot
mkdir -p /boot/once; cp /boot/bzImage /boot/once/bzImage
sh -n /etc/init.d/S50zuktest && sh -n /etc/init.d/S00aramboot && echo "install OK"
INSTALL
sshpass -p openhc ssh $OPTS "$H" "cat > /root/ea1-install2.sh" < /tmp/ea1-install2.sh 2>&1 | grep -v "Permanently added"
ssh_ 'sh /root/ea1-install2.sh 2>&1'

echo "=== reboot into ramboot ==="
OLD=$(ssh_ 'head -c11 /proc/sys/kernel/random/boot_id')
echo "old boot_id=$OLD"
ssh_ 'sync; (sleep 1; reboot) >/dev/null 2>&1 &'
wait_boot "$OLD" RAMBOOT || { echo "FATAL: ramboot never came up (no smart plug) -- STOP, needs Joseph"; exit 2; }

echo "=== wait for FRESH test.log ($TAG complete) ==="
for i in $(seq 1 60); do
  n=$(ssh_ "grep -c '$TAG complete' /root/uk/test.log 2>/dev/null")
  echo "$n" | grep -q '^[1-9]' && { echo "test complete ($i)"; break; }
  sleep 5
done
echo "=== TEST.LOG ==="
ssh_ 'cat /root/uk/test.log'

echo "=== RESTORE to normal boot ==="
OLD2=$(ssh_ 'head -c11 /proc/sys/kernel/random/boot_id')
ssh_ 'rm -f /boot/once/bzImage /etc/init.d/S50zuktest /etc/init.d/S00aramboot; rmdir /boot/once 2>/dev/null; sync; (sleep 1; reboot) >/dev/null 2>&1 &'
wait_boot "$OLD2" NORMAL || { echo "FATAL: normal boot never came back -- needs Joseph"; exit 2; }
echo "=== post-restore state ==="
ssh_ 'echo "boot=$(grep -o ohc.ramboot /proc/cmdline || echo NORMAL) kver=$(uname -r)"; echo "files s50=$(ls /etc/init.d/S50zuktest 2>/dev/null|wc -l) aram=$(ls /etc/init.d/S00aramboot 2>/dev/null|wc -l) once=$(ls /boot/once/bzImage 2>/dev/null|wc -l)"'
echo "=== DONE ==="
