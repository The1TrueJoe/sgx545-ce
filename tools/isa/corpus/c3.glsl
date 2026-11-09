@frag tx_nd
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy); }
@frag tx_nd_s1
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s1, a.xy); }
@frag tx_nd_s3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s3, a.xy); }
@frag tx_nd_b
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, b.xy); }
@frag tx_nd_zw
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.zw); }
@frag tx_nd_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy); }
@frag tx_nd_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy); }
@frag tx_nd_proj3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2DProj(s, a.xyz); }
@frag tx_nd_proj4
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2DProj(s, a); }
@frag tx_nd_cube
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = textureCube(sc, a.xyz); }
@frag tx_nd_two
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy) * texture2D(s1, b.xy); }
@frag tx_nd_same2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy) * texture2D(s, b.xy); }
@frag tx_dep
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy); }
@frag tx_dep_s1
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s1, a.xy * b.xy); }
@frag tx_dep_s2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s2, a.xy * b.xy); }
@frag tx_dep_s3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s3, a.xy * b.xy); }
@frag tx_dep_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy); }
@frag tx_dep_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy); }
@frag tx_dep_bias
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy, 1.5); }
@frag tx_dep_biasv
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy, c.x); }
@frag tx_nd_bias
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy, 1.5); }
@frag tx_dep_proj3
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2DProj(s, a.xyz * b.xyz); }
@frag tx_dep_proj4
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2DProj(s, a * b); }
@frag tx_dep_projb
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2DProj(s, a.xyz * b.xyz, 1.0); }
@frag tx_dep_cube
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = textureCube(sc, a.xyz * b.xyz); }
@frag tx_dep_cubeb
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = textureCube(sc, a.xyz * b.xyz, 2.0); }
@frag tx_dep_u
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, u.xy); }
@frag tx_dep_uv
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy + u.xy); }
@frag tx_dep_chain
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s1, texture2D(s, a.xy).xy); }
@frag tx_dep_x
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(texture2D(s, a.xy * b.xy).x); }
@frag tx_dep_w
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(texture2D(s, a.xy * b.xy).w); }
@frag tx_dep_add
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy) + c; }
@frag tx_dep_add_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy) + c; }
@frag tx_dep_add_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy) + c; }
@frag tx_dep_two
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = texture2D(s, a.xy * b.xy) * texture2D(s1, c.xy * d.xy); }
@frag tx_dep_if
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = c; if (a.x > b.x) r = texture2D(s, a.xy * b.xy); gl_FragColor = r; }
@frag tx_dep_loop
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = vec4(0.0); for (int i = 0; i < 4; i++) r += texture2D(s, a.xy * float(i)); gl_FragColor = r; }
@vert vtx_lod
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p + texture2DLod(s, q.xy, 0.0); }
@vert vtx_lod2
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p + texture2DLod(s, q.xy, r.x); }
@vert vtx_tex
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p + texture2D(s, q.xy); }
@vert vtx_projlod
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p + texture2DProjLod(s, q.xyz, 1.0); }
@frag fl_gt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x > b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_gt0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x > 0.0) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_gt_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x > b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_gt_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x > b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_gt_y
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.y > b.z) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_lt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x < b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_lt0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x < 0.0) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_lt_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x < b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_lt_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x < b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_lt_y
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.y < b.z) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ge
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x >= b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ge0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x >= 0.0) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ge_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x >= b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ge_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x >= b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ge_y
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.y >= b.z) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_le
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x <= b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_le0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x <= 0.0) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_le_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x <= b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_le_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x <= b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_le_y
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.y <= b.z) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_eq
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x == b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_eq0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x == 0.0) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_eq_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x == b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_eq_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x == b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_eq_y
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.y == b.z) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ne
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x != b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ne0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x != 0.0) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ne_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x != b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ne_l
precision lowp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x != b.x) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_ne_y
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.y != b.z) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_and
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x > b.x && a.y > b.y) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_or
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x > b.x || a.y > b.y) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_not
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (!(a.x > b.x)) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_any
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (any(greaterThan(a, b))) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_all
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (all(greaterThan(a, b))) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_bu
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (bu) gl_FragColor = c; else gl_FragColor = d; }
@frag fl_nested
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = d; if (a.x > b.x) { r = c * d; if (a.y > b.y) r = r * a; else r = r + b; } gl_FragColor = r; }
@frag fl_big
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = d; if (a.x > b.x) { r = sin(c) * d + exp2(a) * log2(b) / c; r = r * r + sqrt(a); } else { r = cos(d) * a; } gl_FragColor = r; }
@frag fl_sel
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = (a.x > b.x) ? c : d; }
@frag fl_vsel
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = mix(c, d, vec4(greaterThan(a, b))); }
@frag dc_always
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { discard; }
@frag dc_gt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x > b.x) discard; gl_FragColor = c; }
@frag dc_lt0
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x < 0.0) discard; gl_FragColor = c; }
@frag dc_eq
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { if (a.x == b.x) discard; gl_FragColor = c; }
@frag dc_tex
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 t = texture2D(s, a.xy); if (t.w < 0.5) discard; gl_FragColor = t; }
@frag dc_tex_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 t = texture2D(s, a.xy); if (t.w < 0.5) discard; gl_FragColor = t; }
@frag dc_late
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = a * b; if (r.x > c.x) discard; gl_FragColor = r + d; }
@frag lp_s4
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = vec4(0.0); for (int i = 0; i < 4; i++) r += a * b; gl_FragColor = r; }
@frag lp_s32
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = c; for (int i = 0; i < 32; i++) r = r * a + b; gl_FragColor = r; }
@frag lp_s100
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = c; for (int i = 0; i < 100; i++) r = r * a + b; gl_FragColor = r; }
@frag lp_brk
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = vec4(0.0); for (int i = 0; i < 16; i++) { if (r.x > a.x) break; r += b; } gl_FragColor = r; }
@frag lp_cont
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = vec4(0.0); for (int i = 0; i < 16; i++) { if (r.x > a.x) continue; r += b; } gl_FragColor = r; }
@frag lp_n
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = c; for (int i = 0; i < 64; i++) { if (i >= n) break; r = r * a + b; } gl_FragColor = r; }
@frag lp_while
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = c; int i = 0; while (r.x < a.x) { r += b; i++; if (i > 50) break; } gl_FragColor = r; }
@frag lp_nest
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = c; for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) r = r * a + b; gl_FragColor = r; }
@frag lp_fi
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = c; for (float x = 0.0; x < 10.0; x += 1.0) r = r * a + x; gl_FragColor = r; }
@frag fn_call
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
vec4 g(vec4 x, vec4 y) { if (x.x > y.x) return x * y; return x + y; }
void main() { gl_FragColor = g(a, b) + g(c, d); }
@frag dsx
#extension GL_OES_standard_derivatives : enable
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = dFdx(a); }
@frag dsy
#extension GL_OES_standard_derivatives : enable
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = dFdy(a); }
@frag fwidth
#extension GL_OES_standard_derivatives : enable
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = fwidth(a); }
@frag in_add
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i + j); }
@frag in_add_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i + j); }
@frag is_add
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(i + j)); }
@frag in_sub
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i - j); }
@frag in_sub_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i - j); }
@frag is_sub
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(i - j)); }
@frag in_mul
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i * j); }
@frag in_mul_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i * j); }
@frag is_mul
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(i * j)); }
@frag in_div
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i / j); }
@frag in_div_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i / j); }
@frag is_div
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(i / j)); }
@frag in_neg
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(-i); }
@frag in_neg_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(-i); }
@frag is_neg
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(-i)); }
@frag in_addk
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i + 3); }
@frag in_addk_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i + 3); }
@frag is_addk
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(i + 3)); }
@frag in_mulk
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i * 5); }
@frag in_mulk_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i * 5); }
@frag is_mulk
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(i * 5)); }
@frag in_divk
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i / 4); }
@frag in_divk_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i / 4); }
@frag is_divk
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(i / 4)); }
@frag in_mulv
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i * n); }
@frag in_mulv_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { ivec4 i = ivec4(a); ivec4 j = ivec4(b); gl_FragColor = vec4(i * n); }
@frag is_mulv
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); int nn = n; gl_FragColor = vec4(float(i * nn)); }
@frag cv_ftoi
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(ivec4(a)); }
@frag cv_ftoi_s
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(float(int(a.x))); }
@frag cv_itof_u
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(float(n)); }
@frag cv_itof
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(float(n * int(a.x))); }
@frag cv_btof
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(float(a.x > b.x)); }
@frag cv_ftob
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(bvec4(a)); }
@frag cv_ftoi_m
precision mediump float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(ivec4(a)); }
@frag ic_lt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); if (i < j) gl_FragColor = c; else gl_FragColor = d; }
@frag ic_le
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); if (i <= j) gl_FragColor = c; else gl_FragColor = d; }
@frag ic_gt
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); if (i > j) gl_FragColor = c; else gl_FragColor = d; }
@frag ic_ge
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); if (i >= j) gl_FragColor = c; else gl_FragColor = d; }
@frag ic_eq
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); if (i == j) gl_FragColor = c; else gl_FragColor = d; }
@frag ic_ne
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); int j = int(b.x); if (i != j) gl_FragColor = c; else gl_FragColor = d; }
@frag ic_eqv
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(equal(ivec4(a), ivec4(b))); }
@frag ic_ltv
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(lessThan(ivec4(a), ivec4(b))); }
@frag bo_and
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { bool x = a.x > b.x; bool y = a.y > b.y; gl_FragColor = vec4(x && y); }
@frag bo_or
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { bool x = a.x > b.x; bool y = a.y > b.y; gl_FragColor = vec4(x || y); }
@frag bo_xor
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { bool x = a.x > b.x; bool y = a.y > b.y; gl_FragColor = vec4(x ^^ y); }
@frag bo_not
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { bvec4 x = greaterThan(a, b); gl_FragColor = vec4(not(x)); }
@frag bo_bu
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(bu && a.x > b.x); }
@frag in_mod
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { int i = int(a.x); gl_FragColor = vec4(float(i - (i / 7) * 7)); }
@frag ix_k
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
uniform vec4 ua[16]; uniform mat4 mm;
void main() { gl_FragColor = a * ua[3]; }
@frag ix_n
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
uniform vec4 ua[16]; uniform mat4 mm;
void main() { gl_FragColor = a * ua[n]; }
@frag ix_v
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
uniform vec4 ua[16]; uniform mat4 mm;
void main() { gl_FragColor = a * ua[int(b.x)]; }
@frag ix_v1
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
uniform vec4 ua[16]; uniform mat4 mm;
void main() { gl_FragColor = a * ua[int(b.x) + 1]; }
@frag ix_loop
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
uniform vec4 ua[16]; uniform mat4 mm;
void main() { vec4 r = vec4(0.0); for (int i = 0; i < 16; i++) r += ua[i] * a; gl_FragColor = r; }
@frag ix_comp
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 r = a; r[int(b.x)] = 1.0; gl_FragColor = r; }
@frag ix_rcomp
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = vec4(a[int(b.x)]); }
@frag ix_tmparr
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 t[4]; t[0] = a; t[1] = b; t[2] = c; t[3] = d; gl_FragColor = t[int(a.x)]; }
@frag ix_tmpw
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { vec4 t[4]; t[0] = a; t[1] = b; t[2] = c; t[3] = d; t[int(a.y)] = u; gl_FragColor = t[int(a.x)]; }
@vert vix_k
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p * ka[2]; }
@vert vix_n
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p * ka[n]; }
@vert vix_a
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p * ka[int(pi)]; }
@vert vix_skin
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = mb[int(pi)] * p; }
@vert vix_loop
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { vec4 t = vec4(0.0); for (int i = 0; i < 8; i++) t += ka[i] * p; gl_Position = t; }
@vert vo_pos
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; }
@vert vo_posk
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = vec4(1.0, 2.0, 3.0, 1.0); }
@vert vo_mvp
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = m * p; }
@vert vo_v0
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; o0 = q; }
@vert vo_v01
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; o0 = q; o1 = r; }
@vert vo_v2
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; o2 = q.xy; }
@vert vo_v3
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; o3 = q.x; }
@vert vo_vall
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; o0 = q; o1 = r; o2 = q.zw; o3 = r.y; }
@vert vo_vm
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; om = q; }
@vert vo_ps
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; gl_PointSize = q.x; }
@vert vo_psk
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; gl_PointSize = 4.0; }
@vert vo_psv
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; gl_PointSize = q.x; o0 = r; }
@vert vo_mvpv
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = m * p; o0 = m * q; o2 = r.xy; }
@vert vo_if
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; if (q.x > 0.0) o0 = q; else o0 = r; }
@vert vo_loop
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { vec4 t = p; for (int i = 0; i < n; i++) t = t * q; gl_Position = t; }
@vert vo_vary_f
attribute vec4 p; attribute vec4 q; attribute vec4 r; attribute float pi;
uniform mat4 m; uniform vec4 k; uniform vec4 ka[8]; uniform mat4 mb[4]; uniform int n; uniform float f;
uniform sampler2D s;
varying vec4 o0; varying vec4 o1; varying vec2 o2; varying float o3; varying mediump vec4 om;
void main() { gl_Position = p; o0 = q * f; }
@frag vi_1
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = a; }
@frag vi_2
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = a * b; }
@frag vi_4
precision highp float;
varying vec4 a; varying vec4 b; varying vec4 c; varying vec4 d;
uniform vec4 u; uniform vec4 v; uniform float f; uniform int n; uniform bool bu;
uniform sampler2D s; uniform sampler2D s1; uniform sampler2D s2; uniform sampler2D s3;
uniform samplerCube sc;
void main() { gl_FragColor = a * b + c * d; }
