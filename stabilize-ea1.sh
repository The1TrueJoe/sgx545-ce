#!/bin/zsh
# Reboot the EA1 from its tangled ramboot state back to a clean normal boot and
# verify (new boot_id + no ohc.ramboot). Inline-safe: uses sh_word_split so $OPTS
# expands correctly (unlike bare zsh inline calls).
setopt sh_word_split
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
H=root@10.0.0.110
ssh_() { sshpass -p openhc ssh $OPTS "$H" "$@" 2>&1 | grep -v "Permanently added"; }

OLD=$(ssh_ 'head -c11 /proc/sys/kernel/random/boot_id')
echo "before: boot_id=$OLD cmdline=$(ssh_ 'grep -o ohc.ramboot /proc/cmdline || echo NORMAL')"
echo "=== ensure test files gone ==="
ssh_ 'rm -f /etc/init.d/S50zuktest /etc/init.d/S00aramboot /boot/once/bzImage; rmdir /boot/once 2>/dev/null; sync; echo "s50=$(ls /etc/init.d/S50zuktest 2>/dev/null|wc -l) aram=$(ls /etc/init.d/S00aramboot 2>/dev/null|wc -l) once=$(ls /boot/once/bzImage 2>/dev/null|wc -l)"'
echo "=== reboot to normal ==="
ssh_ 'sync; (sleep 1; reboot) >/dev/null 2>&1 &'
ok=0
for i in $(seq 1 50); do
  sleep 4
  r=$(ssh_ 'echo "$(head -c11 /proc/sys/kernel/random/boot_id) $(grep -o ohc.ramboot /proc/cmdline || echo NORMAL)"' 2>/dev/null)
  bid=$(echo "$r" | awk '{print $1}'); cm=$(echo "$r" | awk '{print $2}')
  if [ -z "$bid" ]; then printf "."; continue; fi
  echo " [$i] boot_id=$bid cmdline=$cm"
  if [ "$cm" = "NORMAL" ] && [ -n "$bid" ] && [ "$bid" != "$OLD" ]; then ok=1; break; fi
done
echo "=== result ok=$ok ==="
ssh_ 'echo "kver=$(uname -r) boot=$(grep -o ohc.ramboot /proc/cmdline || echo NORMAL)"; echo "files: s50=$(ls /etc/init.d/S50zuktest 2>/dev/null|wc -l) aram=$(ls /etc/init.d/S00aramboot 2>/dev/null|wc -l) once=$(ls /boot/once/bzImage 2>/dev/null|wc -l)"; echo "lock=$(cat /tmp/gpu.lock 2>/dev/null || echo none)"'
