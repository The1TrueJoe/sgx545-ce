@frag c10f16_m
precision mediump float;
uniform lowp vec4 q; uniform mediump vec4 sc;
void main() { gl_FragColor = vec4(fract(q.xyz * sc.x), 1.0); }
