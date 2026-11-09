@frag isok_vec4_m
precision mediump float;
varying mediump vec4 v_in0;
bool isOk (vec4 a, vec4 b, float eps) { return all(lessThanEqual(abs(a-b), (eps*abs(b) + eps))); }
void main() { vec4 res = v_in0.wzyx; gl_FragColor = vec4(isOk(res, v_in0.wzyx * 1.01, 0.05) ? 1.0 : 0.0, 0.0, 0.0, 1.0); }
@frag ivec_eq_m
precision mediump float;
varying mediump vec4 v_in0;
bool isOk (ivec3 a, ivec3 b) { return (a == b); }
void main() { ivec3 res = ivec3(v_in0.xyz); gl_FragColor = vec4(isOk(res, ivec3(1, 2, 3)) ? 1.0 : 0.0, any(greaterThan(v_in0, vec4(0.5))) ? 1.0 : 0.0, 0.0, 1.0); }
@frag i2f_highp_m
precision mediump float;
varying mediump vec4 v_in0;
uniform highp int ui;
void main() { highp int i = int(v_in0.x * 10.0) + ui; highp ivec2 j = ivec2(v_in0.yz); mediump vec2 f = vec2(j); gl_FragColor = vec4(float(i), f, 1.0); }
@frag i2f_mediump_m
precision mediump float;
varying mediump vec4 v_in0;
uniform mediump int ui;
void main() { mediump int i = int(v_in0.x * 10.0) + ui; mediump ivec2 j = ivec2(v_in0.yz); gl_FragColor = vec4(float(i), vec2(j), 1.0); }
@frag neg_colour_m
precision mediump float;
uniform float in0;
void main() { gl_FragColor = vec4(-in0); }
@frag ix_tmp_float_dwdlr_m
precision mediump float;
uniform mediump int ui_zero; uniform mediump int ui_one; uniform mediump int ui_two; uniform mediump int ui_three; uniform mediump int ui_four;
varying mediump vec4 v_coords;
void main() { float coords = v_coords.x; float arr[4]; arr[ui_zero] = coords; arr[ui_one] = 0.5; arr[ui_two] = -coords; arr[ui_three] = 1.0;
  float res = 0.0; for (int i = 0; i < ui_four; i++) res += arr[i]; gl_FragColor = vec4(res, 0.0, 0.0, 1.0); }
@frag ix_tmp_vec4_dwdlr_m
precision mediump float;
uniform mediump int ui_zero; uniform mediump int ui_one; uniform mediump int ui_two; uniform mediump int ui_three; uniform mediump int ui_four;
varying mediump vec4 v_coords;
void main() { vec4 coords = v_coords; vec4 arr[4]; arr[ui_zero] = coords; arr[ui_one] = -coords; arr[ui_two] = coords.wzyx; arr[ui_three] = vec4(0.5);
  vec4 res = vec4(0.0); for (int i = 0; i < ui_four; i++) res += arr[i]; gl_FragColor = res; }
@frag ix_tmp_vec4_dwdr_m
precision mediump float;
uniform mediump int ui_zero; uniform mediump int ui_one; uniform mediump int ui_two; uniform mediump int ui_three; uniform mediump int ui_four;
varying mediump vec4 v_coords;
void main() { vec4 coords = v_coords; vec4 arr[4]; arr[ui_zero] = coords; arr[ui_one] = -coords; arr[ui_two] = coords.wzyx; arr[ui_three] = vec4(0.5);
  gl_FragColor = arr[ui_zero] + arr[ui_one] + arr[ui_two] + arr[ui_three]; }
@frag ix_mat2_dwdlr_m
precision mediump float;
uniform mediump int ui_zero; uniform mediump int ui_one; uniform mediump int ui_two; uniform mediump int ui_three; uniform mediump int ui_four;
varying mediump vec4 v_coords;
void main() { mat2 m; m[ui_zero] = v_coords.xy; m[ui_one] = v_coords.zw; vec2 res = vec2(0.0); for (int i = 0; i < ui_two; i++) res += m[i]; gl_FragColor = vec4(res, 0.0, 1.0); }
@frag ix_mat4_dwdr2_m
precision mediump float;
uniform mediump int ui_zero; uniform mediump int ui_one; uniform mediump int ui_two; uniform mediump int ui_three;
varying mediump vec4 v_coords;
void main() { mat4 m; m[ui_zero] = v_coords; m[ui_one] = v_coords.yzwx * 2.0; m[ui_two] = v_coords.zwxy - 1.0; m[ui_three] = v_coords.wxyz + 0.5;
  vec4 r = m[ui_zero] * m[ui_one] + m[ui_two] * m[ui_three]; gl_FragColor = r + m[ui_three].wzyx * m[ui_two]; }
@frag ix_vec2_dlsw_cr_m
precision mediump float;
uniform mediump int ui_zero; uniform mediump int ui_one; uniform mediump int ui_two; uniform mediump int ui_three; uniform mediump int ui_four;
varying mediump vec4 v_coords;
void main() { vec2 tmp; for (int i = 0; i < ui_two; i++) tmp[i] = v_coords[i]; gl_FragColor = vec4(tmp.x, tmp.y, 0.0, 1.0); }
@frag ix_vec2_dlsw_dlsr_m
precision mediump float;
uniform mediump int ui_zero; uniform mediump int ui_one; uniform mediump int ui_two; uniform mediump int ui_three; uniform mediump int ui_four;
varying mediump vec4 v_coords;
void main() { vec2 tmp; for (int i = 0; i < ui_two; i++) tmp[i] = v_coords[i]; vec2 r; for (int i = 0; i < ui_two; i++) r[i] = tmp[i]; gl_FragColor = vec4(r, 0.0, 1.0); }
