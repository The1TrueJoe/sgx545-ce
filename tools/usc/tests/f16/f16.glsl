@frag alpha_m
precision mediump float;
varying vec3 a; varying vec3 b; varying float f;
void main() { gl_FragColor = vec4(a.y + b.x - 0.5, b.y * f, a.z, 1.0); }
@frag k_m
precision mediump float;
varying vec4 a;
void main() { gl_FragColor = vec4(a.x, 0.25, a.z, 0.75); }
