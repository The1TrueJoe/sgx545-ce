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
@frag taps16
precision highp float;
uniform sampler2D t0; uniform vec2 inv; varying vec2 v;
void main()
{
	vec4 c = vec4(0.0);
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			c += texture2D(t0, v + inv * vec2(float(x) - 1.5, float(y) - 1.5));
	gl_FragColor = c * 0.0625;
}
@frag taps16_m
precision mediump float;
uniform sampler2D t0; uniform vec2 inv; varying vec2 v;
void main()
{
	vec4 c = vec4(0.0);
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			c += texture2D(t0, v + inv * vec2(float(x) - 1.5, float(y) - 1.5));
	gl_FragColor = c * 0.0625;
}
@frag taps40
precision highp float;
uniform sampler2D t0; uniform sampler2D t1; uniform sampler2D t2;
uniform sampler2D t3; uniform sampler2D t4; uniform vec2 inv; varying vec2 v;
vec4 ring(sampler2D t, float r)
{
	vec4 c = vec4(0.0);
	for (int k = 0; k < 8; k++) {
		float a = float(k) * 0.785398;
		c += texture2D(t, v + inv * r * vec2(cos(a), sin(a)));
	}
	return c;
}
void main()
{
	gl_FragColor = (ring(t0, 1.0) + ring(t1, 2.0) + ring(t2, 3.0) +
	                ring(t3, 4.0) + ring(t4, 5.0)) * 0.025;
}
