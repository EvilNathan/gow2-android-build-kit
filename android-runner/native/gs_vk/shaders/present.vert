#version 450
// Display composition (gs_hw_backend.cpp kPresentVs): fullscreen triangle.

layout(set = 0, binding = 1) uniform PresentUbo
{
    ivec4 uSrcOffset;
    ivec4 uSrcEdge;
};

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
