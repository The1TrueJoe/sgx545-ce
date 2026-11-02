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
