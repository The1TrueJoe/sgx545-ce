#!/bin/sh
# sgxregs — read the SGX545 identity registers. Runs ON the controller.
#
# The cheapest question worth answering before any driver work: is the GPU
# there, is its BAR mapped, and is the block clocked? If the core is
# power-gated at boot, every later symptom looks like a driver bug and isn't.
#
#   sgxregs.sh            # decode the identity registers
#   sgxregs.sh --raw      # also dump the first 64 words of the register block
#
# Read-only. It touches nothing but four registers, none of which have side
# effects. Register offsets are from services4/srvkm/hwdefs/sgx545defs.h, the
# GPL SGX545 register map Intel published for Cedarview.
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; }
warn() { printf '  \033[33mWARN\033[0m  %s\n' "$1"; }

VENDOR=0x8086
DEVICE=0x089b

# sgx545defs.h
EUR_CR_CLKGATECTL=0x0000
EUR_CR_CORE_ID=0x001C
EUR_CR_CORE_REVISION=0x0020
EUR_CR_SOFT_RESET=0x0080

printf '\n== SGX545 (%s:%s) ==\n' "$VENDOR" "$DEVICE"

# --- locate the device ------------------------------------------------------
SGX=
for d in /sys/bus/pci/devices/*/; do
    [ -r "$d/vendor" ] && [ -r "$d/device" ] || continue
    v=$(cat "$d/vendor"); p=$(cat "$d/device")
    if [ "$v" = "$VENDOR" ] && [ "$p" = "$DEVICE" ]; then SGX="$d"; break; fi
done

if [ -z "$SGX" ]; then
    bad "no $VENDOR:$DEVICE on the PCI bus"
    printf '\n  devices seen on bus 01:\n'
    for d in /sys/bus/pci/devices/0000:01:*/; do
        printf '    %s  %s:%s\n' "$(basename "$d")" \
            "$(cat "$d/vendor" 2>/dev/null)" "$(cat "$d/device" 2>/dev/null)"
    done
    exit 1
fi
ok "found at $(basename "$SGX")"

drv=$(basename "$(readlink "$SGX/driver" 2>/dev/null)" 2>/dev/null)
[ -n "$drv" ] && printf '        bound to: %s\n' "$drv" \
              || printf '        bound to: (nothing)\n'

# --- BAR0 -------------------------------------------------------------------
# /sys/.../resource is one line per BAR: start end flags (hex, 0x-prefixed).
read -r bar0_start bar0_end bar0_flags <<EOT
$(head -1 "$SGX/resource")
EOT
BASE=$(printf '%d' "$bar0_start")
SIZE=$(( $(printf '%d' "$bar0_end") - BASE + 1 ))

if [ "$BASE" -eq 0 ]; then
    bad "BAR0 is unassigned -- firmware did not program it"
    exit 1
fi
ok "BAR0 $bar0_start, $((SIZE / 1024)) KB"

# The register block is at BAR0+0 on CE5300, unlike every other Intel SGX part
# (Poulsbo/Moorestown/Cedarview put it at +0x40000 or +0x80000). If these reads
# come back as nonsense, that assumption is the first thing to doubt.
if [ "$SIZE" -lt 16384 ]; then
    warn "BAR0 smaller than the 16 KB register window the driver expects"
fi

# --- read a register --------------------------------------------------------
# busybox devmem where available; dd on /dev/mem otherwise. Both need
# CONFIG_DEVMEM and CONFIG_STRICT_DEVMEM=n.
have_devmem=0
command -v devmem >/dev/null 2>&1 && have_devmem=1

rd() {  # rd <offset> -> hex value on stdout
    addr=$(( BASE + $1 ))
    if [ "$have_devmem" = 1 ]; then
        devmem "$addr" 32 2>/dev/null
    else
        v=$(dd if=/dev/mem bs=4 count=1 skip=$(( addr / 4 )) 2>/dev/null \
            | od -An -tx4 | tr -d ' \n')
        [ -n "$v" ] && printf '0x%s' "$v"
    fi
}

id=$(rd $EUR_CR_CORE_ID)
rev=$(rd $EUR_CR_CORE_REVISION)
clk=$(rd $EUR_CR_CLKGATECTL)

if [ -z "$id" ] || [ -z "$rev" ]; then
    bad "register reads returned nothing"
    if [ ! -r /dev/mem ]; then
        printf '        /dev/mem is not readable -- the kernel needs\n'
        printf '        CONFIG_DEVMEM=y and CONFIG_STRICT_DEVMEM=n, and this\n'
        printf '        has to run as root.\n'
    elif [ "$have_devmem" = 0 ]; then
        printf '        no devmem(1) and the dd fallback read nothing.\n'
    fi
    exit 1
fi

idv=$(printf '%d' "$id")
revv=$(printf '%d' "$rev")

# --- decode -----------------------------------------------------------------
printf '\n  EUR_CR_CORE_ID        %s\n' "$id"
printf '  EUR_CR_CORE_REVISION  %s\n' "$rev"
printf '  EUR_CR_CLKGATECTL     %s\n' "$clk"

if [ "$idv" -eq 0 ] || [ "$id" = "0xffffffff" ]; then
    bad "core ID reads as $id -- block is unclocked, power-gated, or the"
    printf '        register window is not at BAR0+0 after all.\n'
    exit 1
fi

core_id=$(( (idv >> 16) & 0xffff ))
config=$((  idv         & 0xffff ))
major=$((  (revv >> 16) & 0xff ))
minor=$((  (revv >>  8) & 0xff ))
maint=$((   revv        & 0xff ))

printf '\n  core id     0x%04x  (config 0x%04x)\n' "$core_id" "$config"
printf '  revision    %d.%d.%d\n' "$major" "$minor" "$maint"

# sgxerrata.h spells the EA-3 revision 1014, i.e. major.minor.maintenance
# 1.0.14. The driver is built with SGX_CORE_REV set to match.
printf '  SGX_CORE_REV %d%d%d\n' "$major" "$minor" "$maint"
if [ "$major" = "1" ] && [ "$minor" = "0" ] && [ "$maint" = "14" ]; then
    ok "revision 1014 -- matches the driver's default SGX_CORE_REV"
else
    warn "revision is not 1014; rebuild with SGX_CORE_REV=${major}${minor}${maint}"
    printf '        sgxerrata.h knows 100, 109, 1012, 1013, 10131, 1014, 10141\n'
fi

if [ "$1" = "--raw" ]; then
    printf '\n== first 64 words of the register block ==\n'
    i=0
    while [ $i -lt 64 ]; do
        printf '  +0x%04x  %s\n' $(( i * 4 )) "$(rd $(( i * 4 )))"
        i=$(( i + 1 ))
    done
fi

printf '\n'
