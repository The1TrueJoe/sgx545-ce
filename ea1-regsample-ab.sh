#!/bin/zsh
# EA1 host-side register-sampler A/B: capture named SGX DPM/EVENT registers at ~us
# resolution across 3 consecutive frame-dumps, once under OURS (uk-ro1.bin) and once
# under the DDK microkernel (ddk-fw.bin, 10/10), on the same cold-GPU ramboot rig.
# Diff the two transition streams around each render-end and the next kick to see what
# the DDK's DPM state does between processes that ours doesn't. Read-only, named regs
# only. ALWAYS restores normal boot. Hold ea1.lock around the whole job (caller).
setopt sh_word_split
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
H=root@10.0.0.110
HW=/private/tmp/claude-501/-Users-josephtelaak-Documents-GitHub-openHC--claude-worktrees-optimistic-leakey-b1d663/ca698cd7-46e4-4e13-949b-9c4a8185b579/scratchpad/sgx/hwruns
DDK=/private/tmp/claude-501/-Users-josephtelaak-Documents-GitHub-openHC--claude-worktrees-optimistic-leakey-b1d663/ca698cd7-46e4-4e13-949b-9c4a8185b579/scratchpad/sgx/tools/ddk
OUT=/private/tmp/claude-501/-Users-josephtelaak-Documents-GitHub-openHC--claude-worktrees-optimistic-leakey-b1d663/ca698cd7-46e4-4e13-949b-9c4a8185b579/scratchpad/sgx/docs/log/data
ssh_() { sshpass -p openhc ssh $OPTS "$H" "$@" 2>&1 | grep -v "Permanently added"; }
put()  { sshpass -p openhc ssh $OPTS "$H" "cat > $2" < "$1" 2>&1 | grep -v "Permanently added"; }
wait_boot() { # <old_boot_id> <NORMAL|RAMBOOT>
  old=$1; want=$2; want2=NORMAL; [ "$want" = RAMBOOT ] && want2=ohc.ramboot
  for i in $(seq 1 50); do
    sleep 4
    r=$(ssh_ 'echo "$(head -c11 /proc/sys/kernel/random/boot_id) $(grep -o ohc.ramboot /proc/cmdline || echo NORMAL)"' 2>/dev/null)
    bid=$(echo "$r" | awk '{print $1}'); cm=$(echo "$r" | awk '{print $2}')
    [ -z "$bid" ] && { printf "."; continue; }
    echo " [$i] boot_id=$bid cmdline=$cm want=$want2"
    [ "$cm" = "$want2" ] && [ "$bid" != "$old" ] && { echo "  -> $want OK"; return 0; }
  done
  echo "  -> $want NEVER REACHED"; return 1
}

echo "=== precheck NORMAL boot ==="
pre=$(ssh_ 'grep -o ohc.ramboot /proc/cmdline || echo NORMAL')
echo "$pre" | grep -q NORMAL || { echo "FATAL: not normal boot ($pre)"; exit 3; }
ssh_ "echo ukernel-ea1-regsample > /tmp/gpu.lock"

echo "=== stage regsample + uk-ro1.bin (ddk-fw.bin already on box) ==="
put "$DDK/regsample" /root/uk/regsample
put "$HW/uk-ro1.bin" /root/uk/uk-ro1.bin
put "$HW/uk-ro1.bin.pat" /root/uk/uk-ro1.bin.pat
ssh_ 'chmod +x /root/uk/regsample; md5sum /root/uk/regsample /root/uk/uk-ro1.bin /root/uk/ddk-fw.bin 2>/dev/null | cut -c1-8 | tr "\n" " "; echo'

# installer writes an S50 parametrized by the srvinit mode ($1 = "ours" | "ddk")
mk_install() {
  MODE=$1
  if [ "$MODE" = ddk ]; then SRV='./srvinit -f /root/uk/ddk-fw.bin'; else SRV='./srvinit -o /root/uk/uk-ro1.bin'; fi
  cat > /tmp/ea1-rs-install.sh <<INSTALL
#!/bin/sh
set -e
cat > /etc/init.d/S50zuktest <<'EOS'
#!/bin/sh
[ "\$1" = start ] || exit 0
case " \$(cat /proc/cmdline) " in *ohc.ramboot*) ;; *) exit 0 ;; esac
L=/root/uk/test-$MODE.log; : > "\$L"
C=/root/uk/reg-$MODE.csv
echo "=== S50 regsample $MODE \$(date 2>/dev/null) ===" >> "\$L"
i=0; while [ \$i -lt 15 ] && [ ! -e /dev/dri/card0 ]; do sleep 1; i=\$((i+1)); done
# locate the SGX545 BAR0 (0x8086, CORE_ID high = 0x0115); named-reg reads only
BASE=
for d in /sys/bus/pci/devices/*/; do
  [ "\$(cat \$d/vendor 2>/dev/null)" = 0x8086 ] || continue
  b=\$(awk 'NR==1{print \$1}' "\$d/resource" 2>/dev/null)
  [ "\$b" = 0x0000000000000000 ] && continue
  off=\$(( b + 0x1c )); id=\$(dd if=/dev/mem bs=4 count=1 skip=\$(( off / 4 )) 2>/dev/null | od -An -tx4 | tr -d ' \n')
  case "\$id" in 0115*) BASE=\$b; break;; esac
done
echo "SGX BAR0 base=\$BASE" >> "\$L"
cd /root/uk || exit 0
echo "--- $SRV ---" >> "\$L"
$SRV >> "\$L" 2>&1
rm -f /tmp/rs.stop
if [ -n "\$BASE" ]; then ./regsample "\$BASE" 30000 "\$C" /tmp/rs.stop >> "\$L" 2>&1 & RS=\$!; else echo "NO BASE -- skipping regsample" >> "\$L"; RS=; fi
echo "--- 3 frames (ts=date +%s.%N) ---" >> "\$L"
for f in 1 2 3; do
  echo "FRAME \$f start \$(date +%s.%N 2>/dev/null)" >> "\$L"
  ./frame-dump -o /tmp/f\$f.bin 2>&1 | tail -1 >> "\$L"
  echo "FRAME \$f end \$(date +%s.%N 2>/dev/null)" >> "\$L"
done
touch /tmp/rs.stop; [ -n "\$RS" ] && wait \$RS 2>/dev/null
echo "HWR=\$(dmesg 2>/dev/null | grep -icE 'SGX lockup|Hardware Recovery')" >> "\$L"
echo "regcsv rows=\$(wc -l < \$C 2>/dev/null)" >> "\$L"
echo "=== S50 regsample $MODE complete ===" >> "\$L"
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
sh -n /etc/init.d/S50zuktest && echo "install OK ($MODE)"
INSTALL
  put /tmp/ea1-rs-install.sh /root/ea1-rs-install.sh
  ssh_ 'sh /root/ea1-rs-install.sh 2>&1'
}

run_arm() { # <ours|ddk>
  MODE=$1
  echo "===== ARM: $MODE ====="
  mk_install "$MODE"
  OLD=$(ssh_ 'head -c11 /proc/sys/kernel/random/boot_id')
  ssh_ 'sync; (sleep 1; reboot) >/dev/null 2>&1 &'
  wait_boot "$OLD" RAMBOOT || { echo "FATAL: ramboot for $MODE never came up -- needs Joseph"; return 2; }
  for i in $(seq 1 70); do
    n=$(ssh_ "grep -c 'regsample $MODE complete' /root/uk/test-$MODE.log 2>/dev/null")
    echo "$n" | grep -q '^[1-9]' && { echo "$MODE complete ($i)"; break; }
    sleep 5
  done
  echo "--- test-$MODE.log ---"; ssh_ "cat /root/uk/test-$MODE.log"
  mkdir -p "$OUT"
  sshpass -p openhc scp $OPTS "$H:/root/uk/reg-$MODE.csv" "$OUT/2026-11-04-regsample-$MODE.csv" 2>&1 | grep -v "Permanently added" || \
    ssh_ "cat /root/uk/reg-$MODE.csv" > "$OUT/2026-11-04-regsample-$MODE.csv"
  echo "saved reg-$MODE.csv -> $OUT ($(wc -l < "$OUT/2026-11-04-regsample-$MODE.csv" 2>/dev/null) lines)"
}

run_arm ours || exit 2
run_arm ddk  || exit 2

echo "=== RESTORE normal boot ==="
OLD2=$(ssh_ 'head -c11 /proc/sys/kernel/random/boot_id')
ssh_ 'rm -f /boot/once/bzImage /etc/init.d/S50zuktest /etc/init.d/S00aramboot; rmdir /boot/once 2>/dev/null; sync; (sleep 1; reboot) >/dev/null 2>&1 &'
wait_boot "$OLD2" NORMAL || { echo "FATAL: normal boot never came back -- needs Joseph"; exit 2; }
ssh_ 'echo "post: boot=$(grep -o ohc.ramboot /proc/cmdline||echo NORMAL) s50=$(ls /etc/init.d/S50zuktest 2>/dev/null|wc -l) once=$(ls /boot/once/bzImage 2>/dev/null|wc -l)"'
echo "=== DONE: reg-ours.csv + reg-ddk.csv in $OUT ==="
