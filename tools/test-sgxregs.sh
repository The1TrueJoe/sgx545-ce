#!/bin/sh
# Self-check for sgxregs.sh. Runs anywhere -- no SGX required.
#
# sgxregs.sh does arithmetic that is easy to get subtly wrong (BAR size from a
# start/end pair, three bit-fields out of the revision word) and it normally
# only runs on hardware we cannot reach from a build machine. So mock the two
# things it reads -- sysfs and devmem -- and assert on the output.
#
#   sh tools/test-sgxregs.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(mktemp -d)
trap 'rm -rf "$R"' EXIT

D="$R/sys/bus/pci/devices/0000:01:02.0"
mkdir -p "$D" "$R/bin"
echo 0x8086 > "$D/vendor"
echo 0x089b > "$D/device"
# BAR0 0xdc000000-0xdcffffff (16 MB), as read off a stock EA-3.
printf '0x00000000dc000000 0x00000000dcffffff 0x0000000000040200\n' > "$D/resource"
for _ in 1 2 3 4 5; do
    printf '0x0000000000000000 0x0000000000000000 0x0000000000000000\n' >> "$D/resource"
done

# CORE_REVISION 0x0001000e == major 1, minor 0, maintenance 14 == "1014".
cat > "$R/bin/devmem" <<'EOS'
#!/bin/sh
case "$1" in
  $((0xdc000000 + 0x1c))) echo 0x00450001 ;;
  $((0xdc000000 + 0x20))) echo 0x0001000e ;;
  *) echo 0x00000000 ;;
esac
EOS
chmod +x "$R/bin/devmem"

sed "s|/sys/bus/pci/devices|$R/sys/bus/pci/devices|g" "$HERE/sgxregs.sh" > "$R/sgxregs.sh"
out=$(PATH="$R/bin:$PATH" sh "$R/sgxregs.sh" 2>&1) || true

fail=0
want() {
    if printf '%s' "$out" | grep -q -- "$1"; then
        printf '  ok    %s\n' "$2"
    else
        printf '  FAIL  %s\n' "$2"; fail=1
    fi
}

printf '\n== sgxregs self-check ==\n'
want 'found at 0000:01:02.0'          'locates the SGX by vendor:device'
want '16384 KB'                       'BAR0 size from start/end pair'
want 'revision    1\.0\.14'           'revision bit-fields decode'
want 'SGX_CORE_REV 1014'              'revision renders as the errata-table spelling'
want 'matches the driver'"'"'s default' 'recognises 1014 as the expected revision'

# An unclocked block reads back all-ones; that has to be caught, not decoded.
cat > "$R/bin/devmem" <<'EOS'
#!/bin/sh
echo 0xffffffff
EOS
chmod +x "$R/bin/devmem"
out=$(PATH="$R/bin:$PATH" sh "$R/sgxregs.sh" 2>&1) || true
want 'unclocked, power-gated'         'all-ones read is reported, not decoded'

printf '\n'
[ "$fail" = 0 ] && { printf 'all checks passed\n\n'; exit 0; }
printf 'FAILURES\n\n'; exit 1
