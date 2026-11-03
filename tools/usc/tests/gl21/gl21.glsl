# desktop GL 2.1 needs (piglit): derivatives, gl_FragDepth
@frag dv_m
#extension GL_OES_standard_derivatives : enable
precision mediump float;
varying vec4 a;
void main() { gl_FragColor = vec4(dFdx(a.xy), dFdy(a.z), fwidth(a.w)); }
@frag dv_dep
#extension GL_OES_standard_derivatives : enable
precision highp float;
varying vec2 a; uniform sampler2D s;
void main() { gl_FragColor = texture2D(s, a) * abs(dFdx(a.x) + dFdy(a.y)); }
@frag fd
#extension GL_EXT_frag_depth : enable
precision highp float;
varying vec4 a;
void main() { gl_FragColor = a; gl_FragDepthEXT = a.z * 0.5; }
@frag fd_m
#extension GL_EXT_frag_depth : enable
precision mediump float;
varying vec4 a;
void main() { gl_FragColor = a; gl_FragDepthEXT = a.w; }
@frag fd_dc
#extension GL_EXT_frag_depth : enable
precision highp float;
varying vec4 a;
void main() { if (a.x > 0.5) discard; gl_FragColor = a; gl_FragDepthEXT = a.y; }
@vert vpack
attribute vec4 p; varying vec2 v0; varying vec2 v1; varying vec2 v2; varying vec2 v3; varying vec2 v4; varying vec2 v5; varying vec2 v6; varying vec2 v7; varying vec2 v8; varying vec2 v9; varying float f0; varying float f1; varying vec3 w;
void main() { gl_Position = p; v0 = p.xy; v1 = p.yz; v2 = p.zw; v3 = p.wx; v4 = p.xz; v5 = p.yw; v6 = p.xx; v7 = p.yy; v8 = p.zz; v9 = p.ww; f0 = p.x + 1.0; f1 = p.y + 2.0; w = p.xyz * 3.0; }
@frag fpack
precision highp float;
varying vec2 v0; varying vec2 v1; varying vec2 v2; varying vec2 v3; varying vec2 v4; varying vec2 v5; varying vec2 v6; varying vec2 v7; varying vec2 v8; varying vec2 v9; varying float f0; varying float f1; varying vec3 w;
void main() { gl_FragColor = vec4(v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9, f0 + f1, w.z); }
@frag fpack_m
precision mediump float;
varying vec2 v0; varying vec2 v1; varying vec2 v2; varying vec2 v3; varying vec2 v4; varying vec2 v5; varying vec2 v6; varying vec2 v7; varying vec2 v8; varying vec2 v9; varying float f0; varying float f1; varying vec3 w;
void main() { gl_FragColor = vec4(v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9, f0 + f1, w.z); }
