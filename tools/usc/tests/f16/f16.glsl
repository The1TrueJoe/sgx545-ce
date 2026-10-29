@frag alpha_m
precision mediump float;
varying vec3 a; varying vec3 b; varying float f;
void main() { gl_FragColor = vec4(a.y + b.x - 0.5, b.y * f, a.z, 1.0); }
@frag k_m
precision mediump float;
varying vec4 a;
void main() { gl_FragColor = vec4(a.x, 0.25, a.z, 0.75); }
@frag texdep_m
precision mediump float;
varying vec4 t; varying vec2 uv; uniform sampler2D s;
void main() { gl_FragColor = texture2D(s, uv * 0.5 + 0.25); }
@frag texdepu_m
precision mediump float;
uniform vec4 u; varying vec2 uv; uniform sampler2D s;
void main() { gl_FragColor = texture2D(s, u.xy * uv); }
@frag texmul_m
precision mediump float;
varying vec4 t; varying vec2 uv; uniform sampler2D s;
void main() { gl_FragColor = texture2D(s, uv) * t; }
@frag hfog_m
precision mediump float;
uniform highp vec4 u; varying vec4 t; varying vec2 uv; uniform sampler2D s;
void main() { gl_FragColor = mix(texture2D(s, uv), u, t.x); }
@frag htex_m
precision mediump float;
varying vec4 t; varying vec2 uv; uniform highp sampler2D s;
void main() { gl_FragColor = texture2D(s, uv) * t; }
@frag hfog2_m
precision mediump float;
uniform highp vec4 u; varying vec4 t; varying vec2 uv; uniform sampler2D s;
void main() { mediump vec4 c = u; gl_FragColor = mix(texture2D(s, uv), c, t.x); }
@frag htex2_m
precision mediump float;
varying vec4 t; varying vec2 uv; uniform highp sampler2D s;
void main() { mediump vec4 x = texture2D(s, uv); gl_FragColor = x * t; }
