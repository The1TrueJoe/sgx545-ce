# on the EA1: gl4es-run-ddk = the runtime's gl4es-run with LIBGL_NOBANNER from the environment
# and, with MC_LINEBUF=1, stdout line-buffered (gl4es' FPS lines arrive as they're printed);
# MC_PRELOAD: more libraries preloaded (tools/trace/pvrtrace-i386.so)
R=/opt/minecraft/rootfs
sed 's/LIBGL_NOBANNER=1/LIBGL_NOBANNER=${LIBGL_NOBANNER:-1}/; s|^exec "\$@"|[ -n "$MC_LINEBUF" ] \&\& export LD_PRELOAD="$LD_PRELOAD /usr/libexec/coreutils/libstdbuf.so" _STDBUF_O=L\n[ -n "$MC_PRELOAD" ] \&\& export LD_PRELOAD="$LD_PRELOAD $MC_PRELOAD"\nexec "$@"|' $R/usr/local/bin/gl4es-run > $R/usr/local/bin/gl4es-run-ddk && chmod +x $R/usr/local/bin/gl4es-run-ddk
tail -3 $R/usr/local/bin/gl4es-run-ddk
