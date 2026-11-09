@frag dep_swz_m
precision mediump float;
uniform sampler2D t0; varying vec2 v;
void main() { gl_FragColor = texture2D(t0, v.yx); }
@frag dep_swzk_m
precision mediump float;
uniform sampler2D t0; varying vec2 v;
void main() { gl_FragColor = texture2D(t0, vec2(v.y, 0.5)); }
@frag dep_swz3_m
precision mediump float;
uniform sampler2D t0; varying vec3 v;
void main() { gl_FragColor = texture2D(t0, v.zx); }
@frag dep_proj3_m
precision mediump float;
uniform sampler2D t0; varying vec2 v;
void main() { gl_FragColor = texture2DProj(t0, vec3(v, 1.5)); }
@frag dep_proj3r_m
precision mediump float;
uniform sampler2D t0; varying vec2 v;
void main() { vec3 c = vec3(v.x, 0.25, 1.5); gl_FragColor = texture2DProj(t0, c.xyz) + texture2DProj(t0, c.yxz); }
@frag dep_projswz_m
precision mediump float;
uniform sampler2D t0; varying vec3 v;
void main() { gl_FragColor = texture2DProj(t0, v.zxy); }
@frag view32_m
precision mediump float;
varying vec2 v;
void main() { vec3 c = vec3(v.x, 0.25, 1.5); highp vec3 h = c; highp float f = dot(h, h) + sin(h.y); gl_FragColor = vec4(f, h.yz * 2.0, c.x); }
@frag face_sel_m
precision mediump float;
varying mediump vec4 fc; varying mediump vec4 bc; uniform sampler2D t0; varying vec2 uv;
void main() { gl_FragColor = texture2D(t0, uv) * (gl_FrontFacing ? fc : bc); }
@frag face_mix_m
precision mediump float;
varying mediump vec4 fc; varying mediump vec4 bc; uniform sampler2D t0; varying vec2 uv;
void main() { vec4 a = gl_FrontFacing ? fc : -bc; gl_FragColor = texture2D(t0, uv) * a + (gl_FrontFacing ? vec4(0.5) : bc.wzyx); }
