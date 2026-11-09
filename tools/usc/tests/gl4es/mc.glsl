@vert mc_terrain
#version 100
#extension GL_EXT_shader_non_constant_global_initializers : enable
precision mediump float;
#define GL4ES
attribute highp vec4 _gl4es_Vertex;
attribute lowp vec4 _gl4es_Color;
attribute highp vec4 _gl4es_MultiTexCoord0;
attribute highp vec4 _gl4es_MultiTexCoord1;
uniform highp mat4 _gl4es_ModelViewMatrix;
uniform highp mat4 _gl4es_ModelViewProjectionMatrix;
struct _gl4es_FogParameters {
    lowp vec4 color;
    mediump float density;
    highp   float start;
    highp   float end;
    highp   float scale;
};
uniform _gl4es_FogParameters _gl4es_Fog;
precision mediump int;
// FPE_Shader generated
varying vec4 Color;
varying mediump float FogF;
varying highp vec4 _gl4es_TexCoord_0;
varying highp vec4 _gl4es_TexCoord_1;
uniform highp mat4 _gl4es_TextureMatrix_1;

void main() {
highp vec4 vertex = _gl4es_ModelViewMatrix * _gl4es_Vertex;
gl_Position = _gl4es_ModelViewProjectionMatrix * _gl4es_Vertex;
Color = _gl4es_Color;
vec4 tmp_tex;
_gl4es_TexCoord_0 = _gl4es_MultiTexCoord0.stpq;
tmp_tex = (_gl4es_TextureMatrix_1 * _gl4es_MultiTexCoord1);
_gl4es_TexCoord_1 = tmp_tex.stpq;
float fog_c = abs(vertex.z);
FogF = clamp((_gl4es_Fog.end - fog_c) * _gl4es_Fog.scale, 0., 1.);
}

@frag mc_terrain
#version 100
#extension GL_EXT_shader_non_constant_global_initializers : enable
precision mediump float;
#define GL4ES
struct _gl4es_FogParameters {
    lowp vec4 color;
    mediump float density;
    highp   float start;
    highp   float end;
    highp   float scale;
};
uniform _gl4es_FogParameters _gl4es_Fog;
precision mediump int;
// FPE_Shader generated
varying vec4 Color;
varying mediump float FogF;
varying highp vec4 _gl4es_TexCoord_0;
uniform sampler2D _gl4es_TexSampler_0;
varying highp vec4 _gl4es_TexCoord_1;
uniform sampler2D _gl4es_TexSampler_1;
uniform float _gl4es_AlphaRef;
void main() {
vec4 fColor = Color;
vec4 texColor0 = texture2DProj(_gl4es_TexSampler_0, _gl4es_TexCoord_0);
vec4 texColor1 = texture2DProj(_gl4es_TexSampler_1, _gl4es_TexCoord_1);
fColor *= texColor0;
fColor *= texColor1;
if (floor(fColor.a*255.) <= _gl4es_AlphaRef) discard;
fColor.rgb = mix(_gl4es_Fog.color.rgb, fColor.rgb, FogF);
gl_FragColor = fColor;
}
@vert mc_texfog
#version 100
#extension GL_EXT_shader_non_constant_global_initializers : enable
precision mediump float;
#define GL4ES
attribute highp vec4 _gl4es_Vertex;
attribute lowp vec4 _gl4es_Color;
attribute highp vec4 _gl4es_MultiTexCoord0;
uniform highp mat4 _gl4es_ModelViewMatrix;
uniform highp mat4 _gl4es_ModelViewProjectionMatrix;
struct _gl4es_FogParameters {
    lowp vec4 color;
    mediump float density;
    highp   float start;
    highp   float end;
    highp   float scale;
};
uniform _gl4es_FogParameters _gl4es_Fog;
precision mediump int;
// FPE_Shader generated
varying vec4 Color;
varying mediump float FogF;
varying highp vec4 _gl4es_TexCoord_0;

void main() {
highp vec4 vertex = _gl4es_ModelViewMatrix * _gl4es_Vertex;
gl_Position = _gl4es_ModelViewProjectionMatrix * _gl4es_Vertex;
Color = _gl4es_Color;
_gl4es_TexCoord_0 = _gl4es_MultiTexCoord0.stpq;
float fog_c = abs(vertex.z);
FogF = clamp((_gl4es_Fog.end - fog_c) * _gl4es_Fog.scale, 0., 1.);
}

@frag mc_texfog
#version 100
#extension GL_EXT_shader_non_constant_global_initializers : enable
precision mediump float;
#define GL4ES
struct _gl4es_FogParameters {
    lowp vec4 color;
    mediump float density;
    highp   float start;
    highp   float end;
    highp   float scale;
};
uniform _gl4es_FogParameters _gl4es_Fog;
precision mediump int;
// FPE_Shader generated
varying vec4 Color;
varying mediump float FogF;
varying highp vec4 _gl4es_TexCoord_0;
uniform sampler2D _gl4es_TexSampler_0;
uniform float _gl4es_AlphaRef;
void main() {
vec4 fColor = Color;
vec4 texColor0 = texture2DProj(_gl4es_TexSampler_0, _gl4es_TexCoord_0);
fColor *= texColor0;
if (floor(fColor.a*255.) <= _gl4es_AlphaRef) discard;
fColor.rgb = mix(_gl4es_Fog.color.rgb, fColor.rgb, FogF);
gl_FragColor = fColor;
}
@vert mc_gui
#version 100
#extension GL_EXT_shader_non_constant_global_initializers : enable
precision mediump float;
#define GL4ES
attribute highp vec4 _gl4es_Vertex;
attribute lowp vec4 _gl4es_Color;
attribute highp vec4 _gl4es_MultiTexCoord0;
uniform highp mat4 _gl4es_ModelViewProjectionMatrix;
precision mediump int;
// FPE_Shader generated
varying vec4 Color;
varying highp vec4 _gl4es_TexCoord_0;

void main() {
gl_Position = _gl4es_ModelViewProjectionMatrix * _gl4es_Vertex;
Color = _gl4es_Color;
_gl4es_TexCoord_0 = _gl4es_MultiTexCoord0.stpq;
}

@frag mc_gui
#version 100
#extension GL_EXT_shader_non_constant_global_initializers : enable
precision mediump float;
#define GL4ES
precision mediump int;
// FPE_Shader generated
varying vec4 Color;
varying highp vec4 _gl4es_TexCoord_0;
uniform sampler2D _gl4es_TexSampler_0;
uniform float _gl4es_AlphaRef;
void main() {
vec4 fColor = Color;
vec4 texColor0 = texture2DProj(_gl4es_TexSampler_0, _gl4es_TexCoord_0);
fColor *= texColor0;
if (floor(fColor.a*255.) <= _gl4es_AlphaRef) discard;
gl_FragColor = fColor;
}
@vert mc_sky
#version 100
#extension GL_EXT_shader_non_constant_global_initializers : enable
precision mediump float;
#define GL4ES
attribute highp vec4 _gl4es_Vertex;
attribute lowp vec4 _gl4es_Color;
uniform highp mat4 _gl4es_ModelViewMatrix;
uniform highp mat4 _gl4es_ModelViewProjectionMatrix;
struct _gl4es_FogParameters {
    lowp vec4 color;
    mediump float density;
    highp   float start;
    highp   float end;
    highp   float scale;
};
uniform _gl4es_FogParameters _gl4es_Fog;
precision mediump int;
// FPE_Shader generated
varying vec4 Color;
varying mediump float FogF;

void main() {
highp vec4 vertex = _gl4es_ModelViewMatrix * _gl4es_Vertex;
gl_Position = _gl4es_ModelViewProjectionMatrix * _gl4es_Vertex;
Color = _gl4es_Color;
float fog_c = abs(vertex.z);
FogF = clamp((_gl4es_Fog.end - fog_c) * _gl4es_Fog.scale, 0., 1.);
}

@frag mc_sky
#version 100
#extension GL_EXT_shader_non_constant_global_initializers : enable
precision mediump float;
#define GL4ES
struct _gl4es_FogParameters {
    lowp vec4 color;
    mediump float density;
    highp   float start;
    highp   float end;
    highp   float scale;
};
uniform _gl4es_FogParameters _gl4es_Fog;
precision mediump int;
// FPE_Shader generated
varying vec4 Color;
varying mediump float FogF;
void main() {
vec4 fColor = Color;
fColor.rgb = mix(_gl4es_Fog.color.rgb, fColor.rgb, FogF);
gl_FragColor = fColor;
}
