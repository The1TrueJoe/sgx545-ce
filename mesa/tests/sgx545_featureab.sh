#!/bin/sh
# M0 features against the DDK (the probe's reference scene with one GL feature
# on): scissor and partial viewports, lines and points, stencil, constant-colour
# blending, render to texture. Each case prints IDENTICAL, or DIFFER with how
# many bytes.  Stops at the first SGX recovery.  CASES="sc1 ln1": only those.
cd /tmp/mesa
printf "#!/bin/sh\nLD_PRELOAD=/opt/sgx-i386/usr/lib/sgx-busidshim.so exec /opt/sgx-i386/lib/ld-linux.so.2 --library-path /opt/sgx-i386/usr/lib:/opt/sgx-i386/lib /tmp/mesa/probe-i386\n" > ddk-probe.sh; chmod +x ddk-probe.sh
R0=$(dmesg | grep -c 'Recovery triggered')
run() {
	name=$1; shift
	if [ -n "$CASES" ]; then case " $CASES " in *" $name "*) ;; *) return ;; esac; fi
	env "$@" PROBE_OUT=/tmp/mesa/f-ddk-$name.rgba ./gpuwatch.sh ./ddk-probe.sh > /tmp/mesa/f-ddk-$name.log 2>&1
	env "$@" LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=/tmp/mesa/f-ours-$name.rgba ./gpuwatch.sh ./probe-x86_64 > /tmp/mesa/f-ours-$name.log 2>&1
	if cmp -s f-ddk-$name.rgba f-ours-$name.rgba; then r=IDENTICAL; else r="DIFFER $(cmp -l f-ddk-$name.rgba f-ours-$name.rgba 2>/dev/null | wc -l) bytes"; fi
	echo "$name: $r ours $(grep -o 'centre rgba([0-9,]*)\|recoveries=[0-9]*\|sgx545:.*' f-ours-$name.log | tr '\n' ' ' | cut -c1-120) ddk $(grep -o 'centre rgba([0-9,]*)' f-ddk-$name.log)"
	if [ "$(dmesg | grep -c 'Recovery triggered')" != "$R0" ]; then
		echo "featureab: SGX recovery in $name: stopping"
		exit 1
	fi
}
# scissor and partial viewports (STATE.md section 3's captures)
run sc1 PROBE_SCISSOR="37 23 101 57"
run sc_tall PROBE_SCISSOR="300 100 120 300"
run sc_zero PROBE_SCISSOR="100 100 0 0"
run sc_full PROBE_SCISSOR="0 0 720 480"
run sc_change PROBE_SCISSOR="200 150 200 100" PROBE_SCISSOR2="300 200 100 150" PROBE_DRAWS=2
run sc_off PROBE_SCISSOR="200 150 200 100" PROBE_NOSCISSOR2=1 PROBE_DRAWS=2
run sc_noclear PROBE_NOCLEAR=1 PROBE_SCISSOR="200 150 200 100" PROBE_PBUFFER="720 480"
run vp1 PROBE_VIEWPORT="100 100 300 200"
run vp_change PROBE_VIEWPORT="100 100 300 200" PROBE_VIEWPORT2="400 200 200 200" PROBE_DRAWS=2
run vp_sc PROBE_VIEWPORT="100 100 300 200" PROBE_SCISSOR="200 150 300 100"
run sc_pb PROBE_PBUFFER="128 64" PROBE_SCISSOR="10 10 50 20"
# lines (GL_LINES 1, LINE_LOOP 2, LINE_STRIP 3)
LV="-0.8 -0.8 0.8 -0.6 0.5 0.7 -0.5 0.2"
run ln1 PROBE_PRIM=1 PROBE_VERTS="$LV"
run ln_strip PROBE_PRIM=3 PROBE_VERTS="$LV -0.1 -0.7 0.7 0.0"
run ln_loop PROBE_PRIM=2 PROBE_VERTS="$LV"
run ln_w4 PROBE_PRIM=1 PROBE_VERTS="$LV" PROBE_LINEWIDTH=4
run ln_w9 PROBE_PRIM=1 PROBE_VERTS="$LV" PROBE_LINEWIDTH=9.5
# stencil
S="8 8 8 8 24 8"
run st_eq PROBE_CONFIG="$S" PROBE_CLEARSTENCIL=3 PROBE_STENCIL="514 3 255 7680 7680 7680 255"
run st_ne PROBE_CONFIG="$S" PROBE_CLEARSTENCIL=3 PROBE_STENCIL="517 3 255 7680 7680 7680 255"
run st_mask PROBE_CONFIG="$S" PROBE_CLEARSTENCIL=0x33 PROBE_STENCIL="514 3 15 7680 7680 7680 255"
run st_draw2 PROBE_CONFIG="$S" PROBE_CLEARSTENCIL=0 PROBE_STENCIL="519 1 255 7680 7680 7681 255" PROBE_DRAWS=2
run st_back PROBE_CONFIG="$S" PROBE_CLEARSTENCIL=3 PROBE_STENCIL="514 3 255 7680 7680 7680 255" PROBE_STENCILBACK="512 5 15 7680 7681 7682 15"
run st_backb PROBE_CONFIG="$S" PROBE_CLEARSTENCIL=3 PROBE_VERTS="-0.5 -0.5 -0.5 0.5 0.5 -0.5" PROBE_STENCIL="512 3 255 7680 7680 7680 255" PROBE_STENCILBACK="514 3 255 7680 7680 7680 255"
# constant-colour blending (CONSTANT_COLOR 32769, 1- 32770, CONSTANT_ALPHA 32771, 1- 32772)
BC="0.25 0.5 0.75 0.375"
C="0.25 1 0.5 0.5"
run k_cc PROBE_COLOR="$C" PROBE_BLENDCOLOR="$BC" PROBE_BLEND="32769 0"
run k_icc PROBE_COLOR="$C" PROBE_BLENDCOLOR="$BC" PROBE_BLEND="32770 1"
run k_ca PROBE_COLOR="$C" PROBE_BLENDCOLOR="$BC" PROBE_BLEND="32771 32772"
run k_dst PROBE_COLOR="$C" PROBE_BLENDCOLOR="$BC" PROBE_BLEND="1 32769"
run k_sub PROBE_COLOR="$C" PROBE_BLENDCOLOR="$BC" PROBE_BLEND="32769 1 32778"
run k_rsub PROBE_COLOR="$C" PROBE_BLENDCOLOR="$BC" PROBE_BLEND="32769 1 32779"
run k_mask PROBE_COLOR="$C" PROBE_BLENDCOLOR="$BC" PROBE_BLEND="32769 0" PROBE_CMASK="1 0 1 1" PROBE_PBUFFER="720 480" PROBE_CONFIG="8 8 8 8 0 0"
# render to texture
run fbo_8888 PROBE_FBO="256 256 rgba8888"
run fbo_d16 PROBE_FBO="256 256 rgba8888" PROBE_FBODS="16 0"
run fbo_565 PROBE_FBO="128 128 rgb565"
run fbo_4444 PROBE_FBO="128 128 rgba4444"
run fbo_5551 PROBE_FBO="128 128 rgba5551"
run fbo_npot PROBE_FBO="100 60 rgba8888"
run fbo_ds PROBE_FBO="128 128 rgba8888" PROBE_FBODS="24 8"
# built-in inputs
FV="attribute vec4 p; void main() { gl_Position = p; }"
run fc PROBE_VS="$FV" PROBE_FS="precision mediump float; void main() { gl_FragColor = vec4(gl_FragCoord.x / 720.0, gl_FragCoord.y / 480.0, gl_FragCoord.z, 1.0); }"
run fc_w PROBE_VS="$FV" PROBE_FS="precision highp float; void main() { gl_FragColor = vec4(gl_FragCoord.x / 720.0, gl_FragCoord.w * 0.5, gl_FragCoord.z, 1.0); }"
# cube maps
CV="attribute vec4 p; attribute vec2 a1; varying vec3 v; void main() { gl_Position = p; v = vec3(p.xy, 1.0); }"
run cube PROBE_TEX=1 PROBE_TEXCUBE=1 PROBE_TEXPAT=1 PROBE_TEXSIZE="16 16" PROBE_ATTRS=2 PROBE_VS="$CV" PROBE_FS="precision mediump float; uniform samplerCube t0; varying vec3 v; void main() { gl_FragColor = textureCube(t0, v); }"
run cube8 PROBE_TEX=1 PROBE_TEXCUBE=1 PROBE_TEXPAT=1 PROBE_TEXSIZE="8 8" PROBE_ATTRS=2 PROBE_VS="$CV" PROBE_FS="precision mediump float; uniform samplerCube t0; varying vec3 v; void main() { gl_FragColor = textureCube(t0, v); }"
run cube_dep PROBE_TEX=1 PROBE_TEXCUBE=1 PROBE_TEXPAT=1 PROBE_TEXSIZE="16 16" PROBE_ATTRS=2 PROBE_VS="$CV" PROBE_FS="precision mediump float; uniform samplerCube t0; varying vec3 v; void main() { gl_FragColor = textureCube(t0, v * vec3(1.0, -1.0, 1.0)) * 0.5; }"
run cube_mip PROBE_TEX=1 PROBE_TEXCUBE=1 PROBE_TEXPAT=1 PROBE_TEXSIZE="16 16" PROBE_MIPMAP=1 PROBE_TEXFILTER="9987 9729" PROBE_ATTRS=2 PROBE_VS="$CV" PROBE_FS="precision mediump float; uniform samplerCube t0; varying vec3 v; void main() { gl_FragColor = textureCube(t0, v); }"
# points (GL_POINTS 0): the size, and sprites
PV="-0.5 -0.5 0.5 -0.25 0.0 0.5 0.25 0.25"
run pt8 PROBE_PRIM=0 PROBE_VERTS="$PV" PROBE_VS="attribute vec4 p; void main() { gl_Position = p; gl_PointSize = 8.0; }"
run pt_var PROBE_PRIM=0 PROBE_VERTS="$PV" PROBE_VS="attribute vec4 p; void main() { gl_Position = p; gl_PointSize = 16.0 + p.x * 16.0; }"
run pt_vary PROBE_PRIM=0 PROBE_VERTS="$PV" PROBE_VS="attribute vec4 p; varying vec2 c; void main() { gl_Position = p; gl_PointSize = 16.0; c = p.xy * 0.5 + 0.5; }" PROBE_FS="precision mediump float; varying vec2 c; void main() { gl_FragColor = vec4(c, 0.0, 1.0); }"
run sprite PROBE_PRIM=0 PROBE_VERTS="$PV" PROBE_VS="attribute vec4 p; void main() { gl_Position = p; gl_PointSize = 32.0; }" PROBE_FS="precision mediump float; void main() { gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0); }"
run sprite_v PROBE_PRIM=0 PROBE_VERTS="$PV" PROBE_VS="attribute vec4 p; varying vec2 c; void main() { gl_Position = p; gl_PointSize = 32.0; c = p.xy * 0.5 + 0.5; }" PROBE_FS="precision mediump float; varying vec2 c; void main() { gl_FragColor = vec4(gl_PointCoord.x, c.y, 0.0, 1.0); }"
# gl_FrontFacing: both windings in one draw, under both glFrontFace settings
FFS="precision mediump float; void main() { gl_FragColor = gl_FrontFacing ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0); }"
BOTH="-0.9 -0.5 -0.1 -0.5 -0.5 0.5 0.1 -0.5 0.5 0.5 0.9 -0.5"
run ff PROBE_VS="$FV" PROBE_FS="$FFS" PROBE_VERTS="$BOTH"
run ff_cw PROBE_VS="$FV" PROBE_FS="$FFS" PROBE_VERTS="$BOTH" PROBE_FRONTFACE=2304
run ff_pb PROBE_VS="$FV" PROBE_FS="$FFS" PROBE_VERTS="$BOTH" PROBE_PBUFFER="720 480" PROBE_CONFIG="8 8 8 8 0 0"
run ff_pbcw PROBE_VS="$FV" PROBE_FS="$FFS" PROBE_VERTS="$BOTH" PROBE_PBUFFER="720 480" PROBE_CONFIG="8 8 8 8 0 0" PROBE_FRONTFACE=2304
