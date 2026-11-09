#!/bin/zsh
# EA1 render_done-on-0x1 HW test (binary uk-ro1.bin, commit 1cccc1d).
# Same proven ramboot rig as the clean3 3/10 baseline: one-shot S00aramboot ->
# cold-GPU /boot/once/bzImage with ohc.ramboot=1, S50zuktest runs srvinit -o +
# 10 frame-dumps, logs rate/HWR/BIF. ALWAYS restores to normal boot at the end.
setopt sh_word_split
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
H=root@10.0.0.110
HW=/private/tmp/claude-501/-Users-josephtelaak-Documents-GitHub-openHC--claude-worktrees-optimistic-leakey-b1d663/ca698cd7-46e4-4e13-949b-9c4a8185b579/scratchpad/sgx/hwruns
BIN=uk-ro1.bin
TAG=ro1-render-on-0x1
ssh_() { for i in 1 2 3 4 5; do out=$(sshpass -p openhc ssh $OPTS "$H" "$@" 2>&1 | grep -v "Permanently added"); echo "$out" | grep -qi "Permission denied" || { echo "$out"; return 0; }; sleep 4; done; echo "$out"; return 1; }
put() { sshpass -p openhc ssh $OPTS "$H" "cat > $2" < "$1" 2>&1 | grep -v "Permanently added"; }
waitdown() { for i in $(seq 1 30); do sshpass -p openhc ssh $OPTS -o ConnectTimeout=4 "$H" true 2>/dev/null || { echo "down ($i)"; return 0; }; sleep 2; done; echo "NEVER WENT DOWN"; return 1; }
waitup() { for i in $(seq 1 40); do sshpass -p openhc ssh $OPTS -o ConnectTimeout=4 "$H" true 2>/dev/null && { echo "up ($i)"; return 0; }; sleep 4; done; echo "NEVER CAME UP"; return 1; }

echo "=== claim lock + confirm box ==="
ssh_ 'echo ukernel-ea1-ro1 > /tmp/gpu.lock; uname -r; grep -o "ohc.ramboot\|ohc_kexec=1" /proc/cmdline | tr "\n" " "; echo; ls /boot/bzImage'

echo "=== stage $BIN + .pat ==="
put "$HW/uk-ro1.bin" /root/uk/uk-ro1.bin
put "$HW/uk-ro1.bin.pat" /root/uk/uk-ro1.bin.pat
echo "staged md5 remote=$(ssh_ "md5sum /root/uk/$BIN | cut -c1-8") local=$(md5 -q "$HW/uk-ro1.bin" | cut -c1-8)"

echo "=== write S50zuktest + S00aramboot, arm /boot/once ==="
cat > /tmp/ea1-install.sh <<INSTALL
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
md5sum /root/uk/$BIN; md5sum /boot/bzImage /boot/once/bzImage | cut -c1-8
INSTALL
put /tmp/ea1-install.sh /root/ea1-install.sh
ssh_ 'sh /root/ea1-install.sh 2>&1'

echo "=== reboot into ramboot ==="
ssh_ 'sync; (sleep 1; reboot) >/dev/null 2>&1 &'
waitdown || { echo "WARN: box did not go down; retry reboot -f"; ssh_ 'sync; reboot -f' 2>/dev/null; waitdown; }
waitup   || { echo "FATAL: box never came back after ramboot (no smart plug) — STOP, needs Joseph"; exit 2; }

echo "=== wait for test.log complete (max ~5 min) ==="
for i in $(seq 1 60); do
  done=$(ssh_ "grep -c 'complete' /root/uk/test.log 2>/dev/null")
  echo "$done" | grep -q '^[1-9]' && { echo "test complete ($i)"; break; }
  sleep 5
done

echo "=== TEST.LOG ==="
ssh_ 'cat /root/uk/test.log'
echo "=== ramboot cmdline confirm ==="
ssh_ 'grep -o "ohc.ramboot" /proc/cmdline || echo "(not ramboot?!)"'

echo "=== RESTORE to normal boot (rm /boot/once, S50, S00aramboot; reboot) ==="
ssh_ 'rm -f /boot/once/bzImage /etc/init.d/S50zuktest /etc/init.d/S00aramboot; rmdir /boot/once 2>/dev/null; sync; (sleep 1; reboot) >/dev/null 2>&1 &'
waitdown || { echo "WARN restore: not down; reboot -f"; ssh_ 'sync; reboot -f' 2>/dev/null; waitdown; }
waitup   || { echo "FATAL: box never came back after restore reboot — needs Joseph"; exit 2; }
echo "=== post-restore state ==="
ssh_ 'echo boot=$(grep -o ohc.ramboot /proc/cmdline || echo NORMAL); echo s50=$(ls /etc/init.d/S50zuktest 2>/dev/null | wc -l) aramboot=$(ls /etc/init.d/S00aramboot 2>/dev/null | wc -l) once=$(ls /boot/once/bzImage 2>/dev/null | wc -l); uname -r'
echo "=== DONE (lock still held: ukernel-ea1-ro1) ==="
