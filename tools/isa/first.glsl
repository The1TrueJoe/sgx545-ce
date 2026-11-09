@frag const_green
precision mediump float;
void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }
@frag const_red
precision mediump float;
void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }
@frag uniform_c
precision mediump float; uniform vec4 c;
void main() { gl_FragColor = c; }
@frag uniform_c_highp
precision highp float; uniform vec4 c;
void main() { gl_FragColor = c; }
@frag add_cc
precision highp float; uniform vec4 c; uniform vec4 d;
void main() { gl_FragColor = c + d; }
@frag mul_cc
precision highp float; uniform vec4 c; uniform vec4 d;
void main() { gl_FragColor = c * d; }
@frag mad_ccc
precision highp float; uniform vec4 c; uniform vec4 d; uniform vec4 e;
void main() { gl_FragColor = c * d + e; }
@vert pos
attribute vec4 p;
void main() { gl_Position = p; }
