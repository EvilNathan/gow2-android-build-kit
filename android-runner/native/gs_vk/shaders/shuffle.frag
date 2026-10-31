#version 450
// Channel shuffle (gs_hw_backend.cpp kShuffleFs).

layout(location = 0) flat in vec2 vOff;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSrc;

layout(set = 0, binding = 6) uniform ShuffleUbo
{
    vec2 uInvSize;
    float uZMax;
    int uSrcZ;
    int uHs, uHd;
    float uScale;
    ivec4 uTexa; // TA0 >> 7, TA1 >> 7, AEM
    vec2 uPad;
};

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) + ivec2(round(vOff * uScale));
    vec4 c;
    if (uSrcZ != 0)
    {
        uint z = uint(clamp(texelFetch(uSrc, p, 0).r, 0.0, 1.0) * uZMax + 0.5);
        c = vec4(float(z & 255u), float((z >> 8) & 255u), float((z >> 16) & 255u), float(z >> 24)) / 255.0;
    }
    else
        c = texelFetch(uSrc, p, 0);
    vec2 h = (uHs == 0) ? c.rg : c.ba;
    uint v = uint(h.x * 255.0 + 0.5) | (uint(h.y * 255.0 + 0.5) << 8);
    uint rgb = v & 0x7FFFu;
    uint a = (uTexa.z != 0 && rgb == 0u) ? 0u : uint((v & 0x8000u) != 0u ? uTexa.y : uTexa.x);
    v = rgb | (a << 15);
    h = vec2(float(v & 255u), float(v >> 8)) / 255.0;
    oColor = (uHd == 0) ? vec4(h, 0.0, 0.0) : vec4(0.0, 0.0, h);
}
