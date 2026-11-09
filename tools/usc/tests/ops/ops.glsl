@frag atan_hp_m
varying highp vec4 v_in0;
void main() { highp float res = atan(v_in0.z); mediump vec4 color = vec4(0.0, 0.0, 0.0, 1.0); color.x = res; color = color * 0.50 + 0.50; gl_FragColor = color; }
@frag atan2_mp_m
varying mediump vec4 v_in0; varying mediump vec4 v_in1;
void main() { mediump vec2 res = atan(v_in0.wy, v_in1.yx); mediump vec4 color = vec4(0.0, 0.0, 0.0, 1.0); color.yz = res; color = color * 0.50 + 0.50; gl_FragColor = color; }
@frag smoothstep_mp_m
varying mediump vec4 v_in0; varying mediump vec4 v_in1; varying mediump vec4 v_in2;
void main() { mediump float res = smoothstep(v_in0.z, v_in1.x, v_in2.y); mediump vec4 color = vec4(0.0, 0.0, 0.0, 1.0); color.x = res; color = color * 0.50 + 0.50; gl_FragColor = color; }
@frag exp2_mp_m
varying mediump vec4 v_in0;
void main() { mediump vec4 res = exp2(v_in0.yzwx); gl_FragColor = res; }
@frag sqrt_mp_m
varying mediump vec4 v_in0;
void main() { mediump vec3 res = sqrt(v_in0.zxy); mediump vec4 color = vec4(0.0, 0.0, 0.0, 1.0); color.xyz = res; color = color * 0.30; gl_FragColor = color; }
@frag tex_bias_m
varying mediump vec2 v_texCoord; varying mediump float v_lodBias;
uniform lowp sampler2D u_sampler;
void main() { gl_FragColor = texture2D(u_sampler, v_texCoord, v_lodBias); }
@frag struct_nested_m
varying mediump vec4 v_coords;
struct T { mediump float a; mediump vec2 b[2]; };
struct S { mediump float a; T b[3]; int c; };
uniform S s[2];
void main() {
    mediump float r = (s[0].b[1].b[0].x + s[1].b[2].b[1].y) * s[0].b[0].a;
    mediump float g = s[1].b[0].b[0].y * s[0].b[2].a * s[1].b[2].a;
    mediump float b = (s[0].b[2].b[1].y + s[0].b[1].b[0].y + s[1].a) * s[0].b[1].a;
    mediump float a = float(s[0].c) + s[1].b[2].a - s[1].b[1].a;
    gl_FragColor = vec4(r, g, b, a); }
@frag mixed_colour_m
precision mediump float;
uniform mediump float e;
uniform highp float h;
varying mediump float f;
void main() { gl_FragColor = vec4(-7.5 - e, h, f * -0.25, 1.0); }
@frag log_const_m
precision mediump float;
varying mediump float a; varying mediump float c;
void main() { gl_FragColor = log(vec4(c, 2.0, a, a)); }
