@frag fc
precision highp float; void main() { gl_FragColor = vec4(gl_FragCoord.xy / 64.0, gl_FragCoord.z, gl_FragCoord.w); }
@frag fcm
precision mediump float; void main() { gl_FragColor = vec4(gl_FragCoord.xy / 64.0, gl_FragCoord.z, 1.0); }
@frag pc
precision mediump float; void main() { gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0); }
@frag ff
precision mediump float; void main() { gl_FragColor = gl_FrontFacing ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0); }
@frag fcv
precision highp float; varying vec4 t; void main() { gl_FragColor = t * 0.5 + vec4(gl_FragCoord.xy / 64.0, 0.0, 0.0); }
