#version 450
// Port of the desktop render-target renderer's fragment shader
// (gs_hw_backend.cpp kFsSource + kReinterpretSource + kFsTail).
// Differences from GL: uDest is an input attachment read with subpassLoad (the
// same pixel: destination alpha test, software blend, Z aliasing), and every
// uniform lives in the std140 DrawUbo.

layout(location = 0) noperspective in vec4 vCol;
layout(location = 1) flat in vec4 vColFlat;
layout(location = 2) noperspective in vec3 vSTQ;
layout(location = 3) noperspective in vec2 vUV;
layout(location = 4) flat in vec4 vUVClamp;

layout(location = 0, index = 0) out vec4 oColor;
layout(location = 0, index = 1) out vec4 oBlend;

layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 0, binding = 1, input_attachment_index = 0) uniform subpassInput uDest;
layout(set = 0, binding = 2) uniform usampler2D uTables;
layout(set = 0, binding = 3) uniform sampler2D uRtColor;
layout(set = 0, binding = 4) uniform sampler2D uPalette;
layout(set = 0, binding = 5) uniform sampler2D uIdxPalette;

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

uint tab(uint i) { return texelFetch(uTables, ivec2(int(i & 2047u), int(i >> 11)), 0).r; }

// sub: where inside texel p the sample falls (0..1). The target is scaled: the host texel under that point is read, never
// leaving the PS2 pixel that owns the texel, so a 1:1 reinterpreting copy (the alpha remaps through T8H of the frame)
// keeps the scaled picture's edges instead of giving every host pixel of a PS2 pixel the same value.
vec4 reinterpretTexel(ivec2 p, vec2 sub)
{
    uint x = uint(p.x), y = uint(p.y);
    uint psx = uint(uTexFmt.x), psy = uint(uTexFmt.y);
    uint pagesPerRow = (uint(uTex0.y) * 64u) >> psx;
    uint page = (uint(uTex0.x) >> 5) + (y >> psy) * pagesPerRow + (x >> psx);
    uint entry = ((uint(uTex0.x) & 31u) << (psx + psy)) + ((y & ((1u << psy) - 1u)) << psx) + (x & ((1u << psx) - 1u));
    uint pixelIndex = (page << uint(uTexFmt.z)) + tab(uint(uTexFmt2.z) + entry);
    uint bitpos = (pixelIndex << uint(uTexFmt.w)) + uint(uTexFmt2.x);
    uint B = uint(uRtFmt.w);
    uint rtIdx = bitpos / B;
    uint within = bitpos % B;
    uint rtBase = uint(uRtFmt2.y) << uint(uRtFmt.z);
    if (rtIdx < rtBase) return vec4(1.0, 0.0, 1.0, 1.0);
    uint rel = rtIdx - rtBase;
    uint rpage = rel >> uint(uRtFmt.z);
    uint inpage = rel & ((1u << uint(uRtFmt.z)) - 1u);
    uint e = tab(uint(uRtFmt2.x) + inpage);
    uint rpsx = uint(uRtFmt.x), rpsy = uint(uRtFmt.y);
    uint fbw = uint(uRtFmt2.z);
    uint px = (rpage % fbw) * (1u << rpsx) + (e & ((1u << rpsx) - 1u));
    uint py = (rpage / fbw) * (1u << rpsy) + (e >> rpsx);
    vec2 hs = vec2(textureSize(uRtColor, 0)) / uRtPx; // host texels per PS2 pixel
    vec2 own = vec2(float(px), float(py));
    ivec2 lo = ivec2(floor(own * hs + 0.001)), hi = max(lo, ivec2(ceil((own + 1.0) * hs - 0.001)) - 1);
    ivec2 hp = clamp(ivec2(floor((own + clamp(sub, 0.0, 1.0)) * hs + 0.001)), lo, hi);
    vec4 c = texelFetch(uRtColor, hp, 0) * 255.0;
    c = floor(c + 0.5);
    uint word;
    if (B == 32u)
        word = uint(c.r) | (uint(c.g) << 8) | (uint(c.b) << 16) | (uint(c.a) << 24);
    else
        word = (c.a >= 128.0 ? 0x8000u : 0u) | ((uint(c.b) >> 3) << 10) | ((uint(c.g) >> 3) << 5) | (uint(c.r) >> 3);
    uint bits = uint(uTexFmt2.y);
    uint value = (bits >= 32u) ? (word >> within) : ((word >> within) & ((1u << bits) - 1u));
    int kind = uTexFmt2.w;
    if (kind == 0) return vec4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), float((value >> 24) & 255u)) / 255.0;
    if (kind == 1) return vec4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), 128.0) / 255.0; // alpha via uRtTex24 path
    if (kind == 2)
    {
        vec3 rgb = vec3(float(value & 31u), float((value >> 5) & 31u), float((value >> 10) & 31u)) * 8.0;
        return vec4(rgb, ((value & 0x8000u) != 0u) ? 128.0 : 0.0) / 255.0;
    }
    return texelFetch(uPalette, ivec2(int(value & 255u), 0), 0);
}

// Paletted textures (T8/T4 and their H variants): uTex holds the indices (R8, cached per VRAM upload), the colour comes
// from uIdxPalette (256x1, cached per CLUT); filtering is done on the looked-up colours like the GS.
vec4 indexedTexel(ivec2 p)
{
    ivec2 size = max(ivec2(uTexDim), ivec2(1));
    p.x = (uWrap.x == 0) ? ((p.x % size.x) + size.x) % size.x : clamp(p.x, 0, size.x - 1);
    p.y = (uWrap.y == 0) ? ((p.y % size.y) + size.y) % size.y : clamp(p.y, 0, size.y - 1);
    int idx = int(texelFetch(uTex, p, 0).r * 255.0 + 0.5);
    return texelFetch(uIdxPalette, ivec2(idx, 0), 0);
}

float clamp255(float v) { return clamp(floor(v), 0.0, 255.0); }

bool alphaPasses(float a)
{
    int ai = int(a);
    switch (uAtst)
    {
    case 0: return false;
    case 1: return true;
    case 2: return ai < uAref;
    case 3: return ai <= uAref;
    case 4: return ai == uAref;
    case 5: return ai >= uAref;
    case 6: return ai > uAref;
    default: return ai != uAref;
    }
}

vec3 blendSel(int sel, vec3 cs, vec3 cd) { return sel == 0 ? cs : sel == 1 ? cd : vec3(0.0); }

void main()
{
    uint zFrag = uint(clamp(gl_FragCoord.z, 0.0, 1.0) * uZAliasMax + 0.5);
    if (uZAlias != 0)
    {
        vec4 d = floor(subpassLoad(uDest) * 255.0 + 0.5);
        uint zd = uint(d.r) | (uint(d.g) << 8) | (uint(d.b) << 16);
        int ztst = uZAlias - 1;
        bool pass = ztst == 1 || (ztst == 2 && zFrag >= zd) || (ztst == 3 && zFrag > zd);
        if (!pass) discard;
    }
    if (uDate != 0)
    {
        bool bit = subpassLoad(uDest).a >= 0.5;
        if (bit != (uDatm != 0)) discard;
    }
    vec4 v = (uIip != 0) ? vCol : vColFlat;
    vec3 rgb = v.rgb;
    float a = v.a;
    if (uTme != 0)
    {
        // U = S/Q with the sign of Q: a negative Q flips both coordinates (the menu Kratos has one)
        float q = (abs(vSTQ.z) < 1e-30) ? 1e-30 : vSTQ.z;
        vec2 uv = (uFst != 0) ? vUV : (vSTQ.xy / q) * uTexDim;
        // at the internal scale a pixel samples between the GS's sample points: keep it inside the range the GS samples
        // (sprites), so filtering never reaches the texel beyond the primitive's last one
        uv = clamp(uv, vUVClamp.xy, vUVClamp.zw);
        vec2 stepX = dFdx(uv) * uPxScale, stepY = dFdy(uv) * uPxScale; // texel change per PS2 pixel
        vec2 hostPx = gl_FragCoord.xy - 0.5;
        vec2 ps2Px = floor(hostPx / uPxScale + 0.001);
        vec2 inPx = (hostPx - ps2Px * uPxScale) / uPxScale;
        vec2 dIn = inPx.x * stepX + inPx.y * stepY;
        vec2 subPx = clamp(mix(dIn, 1.0 + dIn, lessThan(dIn, vec2(0.0))), 0.0, 0.999);
        vec2 uvPs2 = floor((uv - dIn) * 16.0 + 0.015625) / 16.0; // the GS's coordinate at the sample point
        bool anisoX = dot(stepX, stepX) >= dot(stepY, stepY);
        vec2 anisoAxis = (anisoX ? stepX : stepY) / uPxScale;
        float anisoMinor = length(anisoX ? stepY : stepX) / uPxScale;
        int anisoTaps = uAniso > 1 ? clamp(int(ceil(length(anisoAxis) / max(anisoMinor, 1.0) - 0.01)), 1, uAniso) : 1;
        // point sampling picks texel floor(u) of the GS's 1/16 texel coordinate: truncate like the GS (the 1/64 step
        // absorbs float interpolation error, a 1:1 copy lands exactly on texel edges)
        if (uLinear == 0)
            uv = floor(uv * 16.0 + 0.015625) / 16.0;
        vec4 t;
        if (uReinterp != 0)
        {
            bool pt = uLinear == 0;
            ivec2 ip = ivec2(floor(pt ? uvPs2 : uv));
            ip = ivec2(clamp(ip.x, 0, uTex0.z - 1), clamp(ip.y, 0, uTex0.w - 1));
            t = reinterpretTexel(ip, pt ? subPx : uv - floor(uv)) * 255.0;
        }
        else if (uIndexed != 0)
        {
            if (uLinear != 0)
            {
                t = vec4(0.0);
                for (int i = 0; i < anisoTaps; ++i)
                {
                    vec2 f = uv + anisoAxis * ((float(i) + 0.5) / float(anisoTaps) - 0.5) - 0.5;
                    ivec2 i0 = ivec2(floor(f));
                    vec2 w = f - floor(f);
                    t += mix(mix(indexedTexel(i0), indexedTexel(i0 + ivec2(1, 0)), w.x),
                             mix(indexedTexel(i0 + ivec2(0, 1)), indexedTexel(i0 + ivec2(1, 1)), w.x), w.y);
                }
                t = t / float(anisoTaps) * 255.0;
            }
            else
                t = indexedTexel(ivec2(floor(uv))) * 255.0;
        }
        else if (uZTex != 0)
        {
            vec2 zhost = vec2(textureSize(uTex, 0)); // depth is always point sampled: the host texel's centre
            vec2 zk = zhost / uTexSize;
            vec2 zp = (zk.x > 1.0 || zk.y > 1.0) ? floor((floor(uvPs2) + subPx) * zk + 0.001) : floor(uv * zk + 0.001);
            float zf = texture(uTex, (zp + 0.5) / zhost).r;
            uint zi = uint(clamp(zf, 0.0, 1.0) * uZMax + 0.5);
            if (uZTex == 16)
            {
                vec3 rgb = vec3(float(zi & 31u), float((zi >> 5) & 31u), float((zi >> 10) & 31u)) * 8.0;
                t = vec4(rgb, ((zi & 0x8000u) != 0u) ? 128.0 : 0.0);
            }
            else
                t = vec4(float(zi & 255u), float((zi >> 8) & 255u), float((zi >> 16) & 255u), 128.0);
        }
        else if (uLinear == 0)
        {
            // the centre of the host texel (render targets are scaled) inside texel floor(u): never on a texel edge
            vec2 host = vec2(textureSize(uTex, 0));
            vec2 k = host / uTexSize;
            vec2 hp;
            if (k.x > 1.0 || k.y > 1.0)
            {
                vec2 tp = floor(mix(uvPs2, clamp(uvPs2, vec2(0.0), uTexDim - 1.0 / 64.0), uRtClamp));
                hp = floor((tp + subPx) * k + 0.001);
            }
            else
                hp = floor(uv * k + 0.001);
            t = texture(uTex, (hp + 0.5) / host) * 255.0;
        }
        else
        {
            uv = mix(uv, clamp(uv, vec2(0.5), uTexDim - 0.5), uRtClamp);
            vec2 k = vec2(textureSize(uTex, 0)) / uTexSize;
            if (anisoTaps > 1) // only uploaded textures (k = 1)
            {
                t = vec4(0.0);
                for (int i = 0; i < anisoTaps; ++i)
                    t += textureLod(uTex, (uv + anisoAxis * ((float(i) + 0.5) / float(anisoTaps) - 0.5)) / uTexSize, 0.0);
                t = t / float(anisoTaps) * 255.0;
            }
            else
                t = texture(uTex, (uv - 0.5 + 0.5 / k) / uTexSize) * 255.0;
        }
        t = floor(t + 0.5);
        if (uRtTex24 == 1)
            t.a = (uAem != 0 && t.r + t.g + t.b == 0.0) ? 0.0 : uTa0;
        else if (uRtTex24 == 2)
            t.a = (uAem != 0 && t.r + t.g + t.b == 0.0) ? 0.0 : ((t.a >= 128.0) ? uTa1 : uTa0);
        float ta = (uTcc != 0) ? t.a : v.a;
        if (uTfx == 0)
        {
            rgb = vec3(clamp255(t.r * v.r / 128.0), clamp255(t.g * v.g / 128.0), clamp255(t.b * v.b / 128.0));
            a = (uTcc != 0) ? clamp255(t.a * v.a / 128.0) : v.a;
        }
        else if (uTfx == 1)
        {
            rgb = t.rgb;
            a = ta;
        }
        else if (uTfx == 2)
        {
            rgb = vec3(clamp255(t.r * v.r / 128.0 + v.a), clamp255(t.g * v.g / 128.0 + v.a), clamp255(t.b * v.b / 128.0 + v.a));
            a = (uTcc != 0) ? clamp255(t.a + v.a) : v.a;
        }
        else
        {
            rgb = vec3(clamp255(t.r * v.r / 128.0 + v.a), clamp255(t.g * v.g / 128.0 + v.a), clamp255(t.b * v.b / 128.0 + v.a));
            a = ta;
        }
    }
    if (uPass != 0)
    {
        bool pass = (uAte == 0) || alphaPasses(a);
        if (uPass == 1 && !pass) discard;
        if (uPass == 2 && pass) discard;
        if (uPass == 2 && uFailTint != 0) { rgb = vec3(255.0, 0.0, 255.0); a = 128.0; }
    }
    if (uSwBlend != 0)
    {
        vec4 d = floor(subpassLoad(uDest) * 255.0 + 0.5);
        vec3 cs = floor(rgb);
        float c = uAbcd.z == 0 ? a : uAbcd.z == 1 ? d.a : uFix;
        vec3 r = floor((blendSel(uAbcd.x, cs, d.rgb) - blendSel(uAbcd.y, cs, d.rgb)) * c / 128.0) + blendSel(uAbcd.w, cs, d.rgb);
        rgb = (uColClamp != 0) ? clamp(r, 0.0, 255.0) : r - 256.0 * floor(r / 256.0);
    }
    if (uZAlias != 0 && uZAliasWrite != 0)
        rgb = vec3(float(zFrag & 255u), float((zFrag >> 8) & 255u), float((zFrag >> 16) & 255u));
    oColor = vec4(rgb / 255.0, a / 255.0);
    float f = min(a * uBlendScale, 1.0);
    oBlend = vec4(f);
}
