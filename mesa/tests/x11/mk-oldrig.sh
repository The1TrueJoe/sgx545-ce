#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# On the EA: the old stack's benchmark launchers in a fresh openhc-minecraft
# install, next to the install's own (which stay untouched):
#   minecraft-ab   the runtime's `minecraft` with MC_DIR (a game directory of
#                  its own) and MC_GL=ddk (gl4es-run-ddk)
#   gl4es-run-ddk  the runtime's gl4es-run, LIBGL_NOBANNER from the
#                  environment, MC_LINEBUF=1 line-buffers stdout (mk-ddkrun.sh)
#   usr/lib/sgx    the box's DDK libraries and powervr.ini hints (mc-session's ddk_sync)
# mc-oldnew.sh old drives the game with the x64 root's xdotool (the runtime
# has none).
set -e
R=/opt/minecraft/rootfs
B=$R/usr/local/bin
sed -e 's|^G=/opt/minecraft$|G=/opt/minecraft GD=${MC_DIR:-/opt/minecraft}|' \
	-e 's|^cd "\$G"$|cd "$GD"|' \
	-e 's|--gameDir "\$G"|--gameDir "$GD"|' \
	-e 's|^exec /usr/local/bin/gl4es-run |exec /usr/local/bin/gl4es-run${MC_GL:+-$MC_GL} |' \
	"$B/minecraft" > "$B/minecraft-ab"
chmod 755 "$B/minecraft-ab"
grep -q 'GD=' "$B/minecraft-ab" && grep -q 'gameDir "$GD"' "$B/minecraft-ab" &&
	grep -q 'gl4es-run${MC_GL' "$B/minecraft-ab" || { echo "mk-oldrig: the launcher changed; minecraft-ab not made" >&2; exit 1; }
sed 's/LIBGL_NOBANNER=1/LIBGL_NOBANNER=${LIBGL_NOBANNER:-1}/; s|^exec "\$@"|[ -n "$MC_LINEBUF" ] \&\& export LD_PRELOAD="$LD_PRELOAD /usr/libexec/coreutils/libstdbuf.so" _STDBUF_O=L\nexec "$@"|' \
	"$B/gl4es-run" > "$B/gl4es-run-ddk"
chmod 755 "$B/gl4es-run-ddk"
grep -q _STDBUF_O "$B/gl4es-run-ddk" || { echo "mk-oldrig: gl4es-run changed; gl4es-run-ddk not made" >&2; exit 1; }
# the DDK's libraries and hints in the root, as mc-session does it (its own functions)
eval "$(sed -n '/^log() {/p; /^ddk_sync() {/,/^}/p; /^game_hints() {/,/^}/p' /opt/minecraft/bin/mc-session)"
ddk_sync
ls "$R/usr/lib/sgx/libsrv_um.so"* > /dev/null
echo "mk-oldrig: minecraft-ab, gl4es-run-ddk and the DDK in $R"
