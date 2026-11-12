# QuakeSpasm 0.97 alias models (r_alias.c), desktop built-ins as plain
# varyings and uniforms: an alpha test a uniform gates, between the sample and
# the colour; _rgb with the sample's alpha 1 (an RGB skin's view swizzle),
# where the discard folds away
@frag alias
precision highp float;
uniform sampler2D Tex;
uniform sampler2D FullbrightTex;
uniform bool UseFullbrightTex;
uniform bool UseOverbright;
uniform bool UseAlphaTest;
uniform float FogDensity;
uniform vec4 FogColor;
varying vec4 TexCoord0;
varying vec4 Color;
varying float FogFragCoord;
void main()
{
	vec4 result = texture2D(Tex, TexCoord0.xy);
	if (UseAlphaTest && (result.a < 0.666))
		discard;
	result *= Color;
	if (UseOverbright)
		result.rgb *= 2.0;
	if (UseFullbrightTex)
		result += texture2D(FullbrightTex, TexCoord0.xy);
	result = clamp(result, 0.0, 1.0);
	float fog = exp(-FogDensity * FogDensity * FogFragCoord * FogFragCoord);
	fog = clamp(fog, 0.0, 1.0);
	result = mix(FogColor, result, fog);
	result.a = Color.a;
	gl_FragColor = result;
}
@frag alias_m
precision mediump float;
uniform sampler2D Tex;
uniform sampler2D FullbrightTex;
uniform bool UseFullbrightTex;
uniform bool UseOverbright;
uniform bool UseAlphaTest;
uniform float FogDensity;
uniform vec4 FogColor;
varying vec4 TexCoord0;
varying vec4 Color;
varying float FogFragCoord;
void main()
{
	vec4 result = texture2D(Tex, TexCoord0.xy);
	if (UseAlphaTest && (result.a < 0.666))
		discard;
	result *= Color;
	if (UseOverbright)
		result.rgb *= 2.0;
	if (UseFullbrightTex)
		result += texture2D(FullbrightTex, TexCoord0.xy);
	result = clamp(result, 0.0, 1.0);
	float fog = exp(-FogDensity * FogDensity * FogFragCoord * FogFragCoord);
	fog = clamp(fog, 0.0, 1.0);
	result = mix(FogColor, result, fog);
	result.a = Color.a;
	gl_FragColor = result;
}
@frag alias_rgb
precision highp float;
uniform sampler2D Tex;
uniform bool UseAlphaTest;
uniform float FogDensity;
uniform vec4 FogColor;
varying vec4 TexCoord0;
varying vec4 Color;
varying float FogFragCoord;
void main()
{
	vec4 result = vec4(texture2D(Tex, TexCoord0.xy).rgb, 1.0);
	if (UseAlphaTest && (result.a < 0.666))
		discard;
	result *= Color;
	result = clamp(result, 0.0, 1.0);
	float fog = exp(-FogDensity * FogDensity * FogFragCoord * FogFragCoord);
	result = mix(FogColor, result, clamp(fog, 0.0, 1.0));
	gl_FragColor = result;
}
@vert alias_v
uniform mat4 MVP;
uniform float Blend;
uniform vec3 ShadeVector;
uniform vec4 LightColor;
attribute vec4 TexCoords;
attribute vec4 Pose1Vert;
attribute vec3 Pose1Normal;
attribute vec4 Pose2Vert;
attribute vec3 Pose2Normal;
varying vec4 TexCoord0;
varying vec4 Color;
varying float FogFragCoord;
float r_avertexnormal_dot(vec3 vertexnormal)
{
	float dot = dot(vertexnormal, ShadeVector);
	if (dot < 0.0)
		return 1.0 + dot * (13.0 / 44.0);
	else
		return 1.0 + dot;
}
void main()
{
	TexCoord0 = TexCoords;
	vec4 lerpedVert = mix(vec4(Pose1Vert.xyz, 1.0), vec4(Pose2Vert.xyz, 1.0), Blend);
	gl_Position = MVP * lerpedVert;
	FogFragCoord = gl_Position.w;
	float dot1 = r_avertexnormal_dot(Pose1Normal);
	float dot2 = r_avertexnormal_dot(Pose2Normal);
	Color = LightColor * vec4(vec3(mix(dot1, dot2, Blend)), 1.0);
}
