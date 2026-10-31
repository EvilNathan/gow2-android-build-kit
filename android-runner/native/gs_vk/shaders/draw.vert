#version 450
// Port of the desktop render-target renderer's vertex shader
// (gs_hw_backend.cpp kVsSource). PS2 pixels in, window space out.

layout(location = 0) in vec2 aPos;     // PS2 pixels, centre-adjusted
layout(location = 1) in float aZ;      // 0..1
layout(location = 2) in vec4 aCol;     // 0..255
layout(location = 3) in vec3 aSTQ;
layout(location = 4) in vec2 aUV;      // texels
layout(location = 5) in vec4 aUVClamp; // texels: the range the GS samples (sprites), else unbounded

layout(set = 0, binding = 6) uniform DrawUbo
{
    vec2 uInvSize;
    vec2 uTexSize;
    vec2 uTexDim;
    vec2 uRtClamp;
    float uPxScale;
    float uZAliasMax;
    int uReinterp;
    int uDate, uDatm;
    int uSwBlend, uColClamp;
    ivec4 uAbcd;
    float uFix;
    int uZAlias, uZAliasWrite;
    int uZTex;
    float uZMax;
    int uIndexed, uLinear;
    ivec4 uWrap;
    int uTme, uFst, uTfx, uTcc, uIip;
    int uAniso;
    int uRtTex24;
    float uTa0;
    float uTa1;
    int uAem;
    int uAte, uAtst, uAref;
    int uFailTint;
    int uPass;
    float uBlendScale;
    ivec4 uTexFmt;
    ivec4 uTexFmt2;
    ivec4 uRtFmt;
    ivec4 uRtFmt2;
    ivec4 uTex0;
    vec2 uRtPx;
    int uPad0, uPad1;
};

layout(location = 0) noperspective out vec4 vCol;
layout(location = 1) flat out vec4 vColFlat;
layout(location = 2) noperspective out vec3 vSTQ;
layout(location = 3) noperspective out vec2 vUV;
layout(location = 4) flat out vec4 vUVClamp;

void main()
{
    gl_Position = vec4(aPos * uInvSize * 2.0 - 1.0, aZ * 2.0 - 1.0, 1.0);
    vCol = aCol;
    vColFlat = aCol;
    vSTQ = aSTQ;
    vUV = aUV;
    vUVClamp = aUVClamp;
}
