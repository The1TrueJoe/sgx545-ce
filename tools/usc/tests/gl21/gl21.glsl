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
@vert vp32
attribute vec4 p; varying float f0; varying float f1; varying float f2; varying float f3; varying float f4; varying float f5; varying float f6; varying float f7; varying float f8; varying float f9; varying float f10; varying float f11; varying float f12; varying float f13; varying float f14; varying float f15; varying float f16; varying float f17; varying float f18; varying float f19; varying float f20; varying float f21; varying float f22; varying float f23; varying float f24; varying float f25; varying float f26; varying float f27; varying float f28; varying float f29; varying float f30; varying float f31;
void main() { gl_Position = p; f0 = p.x + 0.0; f1 = p.x + 1.0; f2 = p.x + 2.0; f3 = p.x + 3.0; f4 = p.x + 4.0; f5 = p.x + 5.0; f6 = p.x + 6.0; f7 = p.x + 7.0; f8 = p.x + 8.0; f9 = p.x + 9.0; f10 = p.x + 10.0; f11 = p.x + 11.0; f12 = p.x + 12.0; f13 = p.x + 13.0; f14 = p.x + 14.0; f15 = p.x + 15.0; f16 = p.x + 16.0; f17 = p.x + 17.0; f18 = p.x + 18.0; f19 = p.x + 19.0; f20 = p.x + 20.0; f21 = p.x + 21.0; f22 = p.x + 22.0; f23 = p.x + 23.0; f24 = p.x + 24.0; f25 = p.x + 25.0; f26 = p.x + 26.0; f27 = p.x + 27.0; f28 = p.x + 28.0; f29 = p.x + 29.0; f30 = p.x + 30.0; f31 = p.x + 31.0; }
@frag fp32
precision highp float;
varying float f0; varying float f1; varying float f2; varying float f3; varying float f4; varying float f5; varying float f6; varying float f7; varying float f8; varying float f9; varying float f10; varying float f11; varying float f12; varying float f13; varying float f14; varying float f15; varying float f16; varying float f17; varying float f18; varying float f19; varying float f20; varying float f21; varying float f22; varying float f23; varying float f24; varying float f25; varying float f26; varying float f27; varying float f28; varying float f29; varying float f30; varying float f31;
void main() { gl_FragColor = vec4(f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9 + f10 + f11 + f12 + f13 + f14 + f15 + f16 + f17 + f18 + f19 + f20 + f21 + f22 + f23 + f24 + f25 + f26 + f27 + f28 + f29 + f30 + f31, 0.0, 0.0, 1.0); }
@frag dv_tmp
#extension GL_OES_standard_derivatives : enable
precision highp float;
varying float x; varying float y;
void main() { gl_FragColor = vec4(dFdx(abs(x)) * 20.0, dFdy(y * 2.0) * 10.0, fwidth(x * y), 1.0); }
@frag dv_neg
#extension GL_OES_standard_derivatives : enable
precision highp float;
varying float x; varying float y;
void main() { gl_FragColor = vec4(dFdx(-abs(x)) * 40.0, dFdx(x * 40.0), dFdy(-abs(y)) * 30.0, 1.0); }
@frag dv_tex
#extension GL_OES_standard_derivatives : enable
precision highp float;
varying vec2 a; uniform sampler2D s;
void main() { vec2 t = a * 3.0; gl_FragColor = texture2D(s, t) + vec4(dFdy(t), 0.0, 0.0); }
