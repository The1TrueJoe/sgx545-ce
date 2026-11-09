@vert ix_varr
attribute vec4 p; uniform vec4 a[8]; uniform int i; varying vec4 t;
void main() { gl_Position = p; t = a[i]; }
@vert ix_vattr
attribute vec4 p; attribute vec4 a1; uniform vec4 ka[4]; varying vec4 t;
void main() { gl_Position = p; t = ka[int(a1.x * 3.99)]; }
@frag ix_frag
precision highp float;
uniform vec4 c[4]; uniform float f; varying vec4 t;
void main() { gl_FragColor = c[int(f)] * t; }
@frag ix_fvary
precision highp float;
uniform vec4 c[4]; varying vec4 t;
void main() { gl_FragColor = c[int(t.x * 3.99)]; }
