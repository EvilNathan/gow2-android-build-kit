#version 450
// Channel shuffle (gs_hw_backend.cpp kShuffleVs): moves halfwords between the
// owner pixels of a 32 bit target through a 16 bit view, one quad per 8x8
// owner block.

layout(location = 0) in vec2 aPos;   // destination owner pixel (PS2 px)
layout(location = 1) in vec2 aOff;   // source owner pixel - destination owner pixel

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

layout(location = 0) flat out vec2 vOff;

void main()
{
    gl_Position = vec4(aPos * uInvSize * 2.0 - 1.0, 0.0, 1.0);
    vOff = aOff;
}
