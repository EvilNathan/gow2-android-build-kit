#version 450
// Display composition (gs_hw_backend.cpp kPresentFs): copies one read circuit
// (offset DBX/DBY) into the presentation framebuffer; circuit 1 is blended
// over circuit 2 by fixed-function blending.

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSrc;

layout(set = 0, binding = 1) uniform PresentUbo
{
    ivec4 uSrcOffset; // x, y: source pixel of output pixel 0; z: 1 = CT24 (alpha 0x80); w: 1 = alpha x2 as blend factor
    ivec4 uSrcEdge;   // x: first source row/column shown; the ones before it repeat it
};

void main()
{
    ivec2 size = textureSize(uSrc, 0);
    ivec2 fc = ivec2(gl_FragCoord.xy);
    // uSrcEdge.y: 1 = the present image is the composition pre-rotated
    // 90 degrees CW (the surface declares ROTATE_90, so the compositor
    // rotates the buffer CCW on display and the image lands upright);
    // 2 = pre-rotated 90 degrees CCW. The fetch maps the output pixel
    // back into source space before the DBX/DBY offset is applied.
    ivec2 q = fc;
    if (uSrcEdge.y == 1)
        q = ivec2(fc.y, size.y - 1 - fc.x);
    else if (uSrcEdge.y == 2)
        q = ivec2(size.x - 1 - fc.y, fc.x);
    ivec2 p = max(q + uSrcOffset.xy, ivec2(uSrcEdge.x));
    vec4 c = (p.x < size.x && p.y < size.y) ? texelFetch(uSrc, p, 0) : vec4(0.0);
    float a = (uSrcOffset.z != 0) ? 128.0 / 255.0 : c.a;
    oColor = vec4(c.rgb, (uSrcOffset.w != 0) ? min(1.0, a * 2.0) : a);
}
