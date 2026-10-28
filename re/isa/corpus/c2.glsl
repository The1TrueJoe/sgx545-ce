@frag neg
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = -a * b; }
@frag neg_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = -a * b; }
@frag neg_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = -a * b; }
@frag abs
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = abs(a) * b; }
@frag abs_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = abs(a) * b; }
@frag abs_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = abs(a) * b; }
@frag floor
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = floor(a); }
@frag floor_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = floor(a); }
@frag floor_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = floor(a); }
@frag fract
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = fract(a); }
@frag fract_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = fract(a); }
@frag fract_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = fract(a); }
@frag ceil
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = ceil(a); }
@frag ceil_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = ceil(a); }
@frag ceil_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = ceil(a); }
@frag sqrt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sqrt(a); }
@frag sqrt_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sqrt(a); }
@frag sqrt_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sqrt(a); }
@frag rsq
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = inversesqrt(a); }
@frag rsq_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = inversesqrt(a); }
@frag rsq_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = inversesqrt(a); }
@frag exp2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = exp2(a); }
@frag exp2_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = exp2(a); }
@frag exp2_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = exp2(a); }
@frag log2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = log2(a); }
@frag log2_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = log2(a); }
@frag log2_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = log2(a); }
@frag rcp
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = 1.0 / a; }
@frag rcp_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = 1.0 / a; }
@frag rcp_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = 1.0 / a; }
@frag sin
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sin(a); }
@frag sin_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sin(a); }
@frag sin_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sin(a); }
@frag cos
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = cos(a); }
@frag cos_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = cos(a); }
@frag cos_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = cos(a); }
@frag sign
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sign(a); }
@frag sign_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sign(a); }
@frag sign_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = sign(a); }
@frag sat
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = clamp(a, 0.0, 1.0); }
@frag sat_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = clamp(a, 0.0, 1.0); }
@frag sat_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = clamp(a, 0.0, 1.0); }
@frag norm3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(normalize(a.xyz), 1.0); }
@frag norm3_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(normalize(a.xyz), 1.0); }
@frag norm3_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(normalize(a.xyz), 1.0); }
@frag mov
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a; }
@frag mov_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a; }
@frag mov_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a; }
@frag add
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + b; }
@frag add_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + b; }
@frag add_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + b; }
@frag sub
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a - b; }
@frag sub_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a - b; }
@frag sub_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a - b; }
@frag mul
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b; }
@frag mul_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b; }
@frag mul_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b; }
@frag div
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a / b; }
@frag div_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a / b; }
@frag div_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a / b; }
@frag min
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = min(a, b); }
@frag min_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = min(a, b); }
@frag min_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = min(a, b); }
@frag max
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = max(a, b); }
@frag max_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = max(a, b); }
@frag max_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = max(a, b); }
@frag step
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = step(a, b); }
@frag step_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = step(a, b); }
@frag step_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = step(a, b); }
@frag dp3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(dot(a.xyz, b.xyz)); }
@frag dp3_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(dot(a.xyz, b.xyz)); }
@frag dp3_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(dot(a.xyz, b.xyz)); }
@frag dp4
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(dot(a, b)); }
@frag dp4_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(dot(a, b)); }
@frag dp4_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(dot(a, b)); }
@frag mad
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b + c; }
@frag mad_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b + c; }
@frag mad_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b + c; }
@frag mix
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = mix(a, b, c); }
@frag mix_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = mix(a, b, c); }
@frag mix_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = mix(a, b, c); }
@frag lt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(lessThan(a, b)); }
@frag lt_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(lessThan(a, b)); }
@frag lt_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(lessThan(a, b)); }
@frag pow
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = pow(a, b); }
@frag pow_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = pow(a, b); }
@frag pow_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = pow(a, b); }
@frag mod
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = mod(a, b); }
@frag mod_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = mod(a, b); }
@frag mod_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = mod(a, b); }
@frag satmul
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = clamp(a * b, 0.0, 1.0); }
@frag satmul_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = clamp(a * b, 0.0, 1.0); }
@frag satmul_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = clamp(a * b, 0.0, 1.0); }
@frag s_add
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x + b.x); }
@frag s_add_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x + b.x); }
@frag s_mul
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * b.x); }
@frag s_mul_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * b.x); }
@frag s_mad
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * b.x + c.x); }
@frag s_mad_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * b.x + c.x); }
@frag s_rcp
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(1.0 / a.x); }
@frag s_rcp_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(1.0 / a.x); }
@frag s_rsq
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(inversesqrt(a.x)); }
@frag s_rsq_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(inversesqrt(a.x)); }
@frag s_exp2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(exp2(a.x)); }
@frag s_exp2_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(exp2(a.x)); }
@frag s_log2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(log2(a.x)); }
@frag s_log2_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(log2(a.x)); }
@frag s_min
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(min(a.x, b.x)); }
@frag s_min_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(min(a.x, b.x)); }
@frag s_max
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(max(a.x, b.x)); }
@frag s_max_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(max(a.x, b.x)); }
@frag s_sub
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x - b.x); }
@frag s_sub_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x - b.x); }
@frag s_frc
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(fract(a.x)); }
@frag s_frc_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(fract(a.x)); }
@frag s_dp3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(dot(a.xyz, b.xyz)); }
@frag s_dp3_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(dot(a.xyz, b.xyz)); }
@frag s_mul_yz
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.y * b.z); }
@frag s_mul_yz_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.y * b.z); }
@frag s_mul_wx
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.w * b.x); }
@frag s_mul_wx_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.w * b.x); }
@frag s_neg
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(-a.x * b.x); }
@frag s_neg_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(-a.x * b.x); }
@frag s_abs
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(abs(a.x) * b.x); }
@frag s_abs_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(abs(a.x) * b.x); }
@frag s_mulc
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * 3.0); }
@frag s_mulc_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * 3.0); }
@frag s_addc
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x + 0.5); }
@frag s_addc_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x + 0.5); }
@frag s_mulu
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * u.x); }
@frag s_mulu_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * u.x); }
@frag s_mulu_y
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * u.y); }
@frag s_mulu_y_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * u.y); }
@frag s_mulv
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * v.z); }
@frag s_mulv_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * v.z); }
@frag r_aa
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * a.x); }
@frag r_aa
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * a.y); }
@frag rv_aa
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * a; }
@frag r_ab
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * b.x); }
@frag r_ba
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(b.x * a.y); }
@frag rv_ab
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b; }
@frag r_ac
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * c.x); }
@frag r_ca
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(c.x * a.y); }
@frag rv_ac
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * c; }
@frag r_ad
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * d.x); }
@frag r_da
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(d.x * a.y); }
@frag rv_ad
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * d; }
@frag r_ae
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * e.x); }
@frag r_ea
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(e.x * a.y); }
@frag rv_ae
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * e; }
@frag r_af
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * f.x); }
@frag r_fa
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(f.x * a.y); }
@frag rv_af
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * f; }
@frag r_ag
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * g.x); }
@frag r_ga
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(g.x * a.y); }
@frag rv_ag
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * g; }
@frag r_ah
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(a.x * h.x); }
@frag r_ha
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = vec4(h.x * a.y); }
@frag rv_ah
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * h; }
@frag sw_xyzw
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.xyzw * b; }
@frag sw2_xyzw
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.xyzw; }
@frag swm_xyzw
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.xyzw * b; }
@frag sw_yzwx
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.yzwx * b; }
@frag sw2_yzwx
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.yzwx; }
@frag swm_yzwx
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.yzwx * b; }
@frag sw_xxxx
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.xxxx * b; }
@frag sw2_xxxx
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.xxxx; }
@frag swm_xxxx
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.xxxx * b; }
@frag sw_wzyx
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.wzyx * b; }
@frag sw2_wzyx
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.wzyx; }
@frag swm_wzyx
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.wzyx * b; }
@frag sw_zxyw
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.zxyw * b; }
@frag sw2_zxyw
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.zxyw; }
@frag swm_zxyw
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.zxyw * b; }
@frag sw_yyyy
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.yyyy * b; }
@frag sw2_yyyy
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.yyyy; }
@frag swm_yyyy
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.yyyy * b; }
@frag sw_wwww
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.wwww * b; }
@frag sw2_wwww
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.wwww; }
@frag swm_wwww
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.wwww * b; }
@frag sw_xyxy
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.xyxy * b; }
@frag sw2_xyxy
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.xyxy; }
@frag swm_xyxy
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.xyxy * b; }
@frag sw_zzzz
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.zzzz * b; }
@frag sw2_zzzz
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.zzzz; }
@frag swm_zzzz
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.zzzz * b; }
@frag sw_xwyz
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.xwyz * b; }
@frag sw2_xwyz
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * b.xwyz; }
@frag swm_xwyz
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a.xwyz * b; }
@frag wm_x
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.x = (a * c).x; gl_FragColor = r; }
@frag wmm_x
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.x = (a * c).x; gl_FragColor = r; }
@frag wm_y
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.y = (a * c).y; gl_FragColor = r; }
@frag wmm_y
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.y = (a * c).y; gl_FragColor = r; }
@frag wm_z
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.z = (a * c).z; gl_FragColor = r; }
@frag wmm_z
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.z = (a * c).z; gl_FragColor = r; }
@frag wm_w
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.w = (a * c).w; gl_FragColor = r; }
@frag wmm_w
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.w = (a * c).w; gl_FragColor = r; }
@frag wm_xy
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.xy = (a * c).xy; gl_FragColor = r; }
@frag wmm_xy
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.xy = (a * c).xy; gl_FragColor = r; }
@frag wm_xz
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.xz = (a * c).xz; gl_FragColor = r; }
@frag wmm_xz
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.xz = (a * c).xz; gl_FragColor = r; }
@frag wm_yw
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.yw = (a * c).yw; gl_FragColor = r; }
@frag wmm_yw
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.yw = (a * c).yw; gl_FragColor = r; }
@frag wm_xyz
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.xyz = (a * c).xyz; gl_FragColor = r; }
@frag wmm_xyz
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.xyz = (a * c).xyz; gl_FragColor = r; }
@frag wm_yzw
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.yzw = (a * c).yzw; gl_FragColor = r; }
@frag wmm_yzw
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = b; r.yzw = (a * c).yzw; gl_FragColor = r; }
@frag kmul_k0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 0.0; }
@frag kadd_k0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + 0.0; }
@frag kmulm_k0
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 0.0; }
@frag kmul_k1
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 1.0; }
@frag kadd_k1
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + 1.0; }
@frag kmulm_k1
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 1.0; }
@frag kmul_k2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 2.0; }
@frag kadd_k2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + 2.0; }
@frag kmulm_k2
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 2.0; }
@frag kmul_kh
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 0.5; }
@frag kadd_kh
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + 0.5; }
@frag kmulm_kh
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 0.5; }
@frag kmul_kn1
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * -1.0; }
@frag kadd_kn1
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + -1.0; }
@frag kmulm_kn1
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * -1.0; }
@frag kmul_k3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 3.0; }
@frag kadd_k3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + 3.0; }
@frag kmulm_k3
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 3.0; }
@frag kmul_k100
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 100.0; }
@frag kadd_k100
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + 100.0; }
@frag kmulm_k100
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 100.0; }
@frag kmul_kodd
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 3.25; }
@frag kadd_kodd
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + 3.25; }
@frag kmulm_kodd
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 3.25; }
@frag kmul_kpi
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 3.14159; }
@frag kadd_kpi
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + 3.14159; }
@frag kmulm_kpi
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * 3.14159; }
@frag kvec
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * vec4(1.0, 2.0, 3.0, 4.0); }
@frag u_mul
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * u; }
@frag u_add
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a + u; }
@frag v_mul
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * v; }
@frag uv_mad
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * u + v; }
@frag u_mul_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { gl_FragColor = a * u; }
@frag if_gt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { if (a.x > b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag if_lt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { if (a.x < b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag if_eq
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { if (a.x == b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag if_ge
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { if (a.x >= b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag if_ne
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { if (a.x != b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag if_z
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { if (a.x > 0.0) gl_FragColor = c; else gl_FragColor = d; }
@frag if_mul
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = c; if (a.x > b.x) r = c * d; gl_FragColor = r; }
@frag discard
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { if (a.x > b.x) discard; gl_FragColor = c; }
@frag loop_v
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
void main() { vec4 r = vec4(0.0); for (int i = 0; i < 8; i++) { if (r.x > a.x) break; r += b; } gl_FragColor = r; }
@frag t_dep
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
uniform sampler2D s; uniform sampler2D s2;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy); }
@frag t_two
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
uniform sampler2D s; uniform sampler2D s2;
void main() { gl_FragColor = texture2D(s, a.xy) + texture2D(s2, b.xy); }
@frag t_lod
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
uniform sampler2D s; uniform sampler2D s2;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy, 2.0); }
@frag t_mul
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;
uniform vec4 u; uniform vec4 v;
uniform sampler2D s; uniform sampler2D s2;
void main() { gl_FragColor = texture2D(s, a.xy) * b; }
@vert v_mvp
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = m * p; }
@vert v_add
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = p + q; }
@vert v_mul
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = p * q; }
@vert v_mad
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = p * q + n; }
@vert v_dp4
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = vec4(dot(p, q)); }
@vert v_rsq
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = inversesqrt(p); }
@vert v_vary
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = p; o = q * k; }
@vert v_k
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = p * k; }
@vert v_norm
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = vec4(normalize(p.xyz), 1.0); }
@vert v_psize
attribute vec4 p; attribute vec4 q; attribute vec4 n; uniform mat4 m; uniform vec4 k; varying vec4 o;
void main() { gl_Position = p; gl_PointSize = q.x; }
