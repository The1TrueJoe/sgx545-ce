@frag neg
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = -u; }
@frag abs
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = abs(u); }
@frag floor
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = floor(u); }
@frag fract
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = fract(u); }
@frag ceil
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = ceil(u); }
@frag sqrt
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = sqrt(u); }
@frag rsq
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = inversesqrt(u); }
@frag exp2
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = exp2(u); }
@frag log2
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = log2(u); }
@frag exp
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = exp(u); }
@frag log
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = log(u); }
@frag sin
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = sin(u); }
@frag cos
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = cos(u); }
@frag sign
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = sign(u); }
@frag rcp
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = 1.0 / u; }
@frag sat
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = clamp(u, 0.0, 1.0); }
@frag norm3
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(normalize(u.xyz), 1.0); }
@frag len3
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(length(u.xyz)); }
@frag add
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u + v; }
@frag add_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u + v; }
@frag add_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u + v; }
@frag sub
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u - v; }
@frag sub_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u - v; }
@frag sub_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u - v; }
@frag mul
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u * v; }
@frag mul_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u * v; }
@frag mul_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u * v; }
@frag div
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u / v; }
@frag div_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u / v; }
@frag div_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u / v; }
@frag min
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = min(u, v); }
@frag min_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = min(u, v); }
@frag min_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = min(u, v); }
@frag max
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = max(u, v); }
@frag max_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = max(u, v); }
@frag max_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = max(u, v); }
@frag step
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = step(u, v); }
@frag step_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = step(u, v); }
@frag step_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = step(u, v); }
@frag dp3
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u.xyz, v.xyz)); }
@frag dp3_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u.xyz, v.xyz)); }
@frag dp3_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u.xyz, v.xyz)); }
@frag dp4
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u, v)); }
@frag dp4_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u, v)); }
@frag dp4_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u, v)); }
@frag pow
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = pow(u, v); }
@frag pow_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = pow(u, v); }
@frag pow_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = pow(u, v); }
@frag mod
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = mod(u, v); }
@frag mod_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = mod(u, v); }
@frag mod_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = mod(u, v); }
@frag mad
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u * v + w; }
@frag mad_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u * v + w; }
@frag mad_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u * v + w; }
@frag mix
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = mix(u, v, w); }
@frag mix_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = mix(u, v, w); }
@frag mix_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = mix(u, v, w); }
@frag lt
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(lessThan(u, v)); }
@frag lt_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(lessThan(u, v)); }
@frag lt_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(lessThan(u, v)); }
@frag dpx
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u.xy, v.xy)); }
@frag dpx_m
precision mediump float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u.xy, v.xy)); }
@frag dpx_l
precision lowp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(dot(u.xy, v.xy)); }
@frag swz_yzwx
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.yzwx; }
@frag swzadd_yzwx
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.yzwx + v; }
@frag swz_xxxx
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.xxxx; }
@frag swzadd_xxxx
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.xxxx + v; }
@frag swz_wzyx
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.wzyx; }
@frag swzadd_wzyx
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.wzyx + v; }
@frag swz_zxyw
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.zxyw; }
@frag swzadd_zxyw
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.zxyw + v; }
@frag swz_yyyy
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.yyyy; }
@frag swzadd_yyyy
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.yyyy + v; }
@frag swz_wwww
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.wwww; }
@frag swzadd_wwww
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.wwww + v; }
@frag swz_xyxy
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.xyxy; }
@frag swzadd_xyxy
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u.xyxy + v; }
@frag c0
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(0.0); }
@frag c1
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(1.0); }
@frag c_half
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(0.5); }
@frag c_2
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(2.0); }
@frag c_mix
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(0.0, 0.5, 1.0, 2.0); }
@frag c_odd
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(3.25, -7.5, 100.0, 0.125); }
@frag wm_x
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(u.x, 0.0, 0.0, 1.0); }
@frag wm_xy
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(u.xy, 0.0, 1.0); }
@frag wm_w
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(0.0, 0.0, 0.0, u.w); }
@frag wm_uv
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = vec4(u.x, v.y, u.z, v.w); }
@frag u_only
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = u; }
@frag v_only
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = v; }
@frag w_only
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { gl_FragColor = w; }
@frag vary
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;
void main() { gl_FragColor = t; }
@frag vary_mul
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;
void main() { gl_FragColor = t * u; }
@frag tex
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, uv); }
@frag tex_dep
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, u.xy); }
@frag tex_bias
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, uv, 1.0); }
@frag tex_proj
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;
void main() { gl_FragColor = texture2DProj(s, t); }
@frag tex_cube
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;
void main() { gl_FragColor = textureCube(sc, t.xyz); }
@frag tex_mul
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
varying vec4 t; varying vec2 uv; uniform sampler2D s; uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, uv) * u; }
@frag discard
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { if (u.x > 0.5) discard; gl_FragColor = v; }
@frag branch
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { if (u.x > 0.0) gl_FragColor = v; else gl_FragColor = w; }
@frag loop4
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { vec4 a = vec4(0.0); for (int i = 0; i < 4; i++) a += u * float(i); gl_FragColor = a; }
@frag loop_dyn
precision highp float;
uniform vec4 u; uniform vec4 v; uniform vec4 w;
void main() { vec4 a = vec4(0.0); for (int i = 0; i < 16; i++) { if (float(i) > u.x) break; a += v; } gl_FragColor = a; }
@vert vpos
attribute vec4 p;
void main() { gl_Position = p; }
@vert vmvp
attribute vec4 p; uniform mat4 m;
void main() { gl_Position = m * p; }
@vert vvary
attribute vec4 p; attribute vec4 c; varying vec4 t;
void main() { gl_Position = p; t = c; }
@vert varr
attribute vec4 p; uniform vec4 a[8]; uniform int i;
void main() { gl_Position = p + a[i]; }
@vert vtex
attribute vec4 p; uniform sampler2D s;
void main() { gl_Position = p + texture2DLod(s, p.xy, 0.0); }
