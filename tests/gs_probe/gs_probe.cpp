// gs_vk probe: offscreen smoke tests for the raw Vulkan GS backend, run on the
// desktop against the SwiftShader ICD (no game ELF, no window).
//
//   gs_probe [case ...]
// Cases:
//   clear    - render target creation + colour clear readback
//   sprite   - an untextured sprite lands in the target and the present path
//   transfer - a host->local transfer invalidates and re-uploads a texture
//
// Build (see scripts/build-gs-probe.ps1): compiles the backend + engine GS
// sources with clang/GCC or MSVC and links the SwiftShader DLL at run time via
// PS2X_VK_ICD.
#include "gs_vk/gs_vk_backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct Ctx
    {
        std::vector<uint8_t> vram;
        GSVkBackend gs;
    };

    bool makeCtx(Ctx &c)
    {
        c.vram.assign(4u * 1024u * 1024u, 0u);
        c.gs.Initialize(c.vram.data(), static_cast<uint32_t>(c.vram.size()));
        return true;
    }

    int failures = 0;

    void expect(bool ok, const char *what)
    {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok)
            ++failures;
    }

    // one sprite in CT32 at fbp 0, colour from r/g/b, full scissor
    GSPrimitiveBatch spriteBatch(float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b, const GSDrawState &s)
    {
        GSPrimitiveBatch batch{};
        batch.vertexCount = 2;
        auto &a = batch.vertices[0];
        auto &c = batch.vertices[1];
        a.x = x0; a.y = y0; a.z = 0.0; a.r = r; a.g = g; a.b = b; a.a = 128; a.q = 1.0f; a.s = 0.f; a.t = 0.f; a.u = 0; a.v = 0;
        c.x = x1; c.y = y1; c.z = 0.0; c.r = r; c.g = g; c.b = b; c.a = 128; c.q = 1.0f; c.s = 0.f; c.t = 0.f; c.u = 0; c.v = 0;
        batch.state = s;
        return batch;
    }

    GSDrawState baseState()
    {
        GSDrawState s{};
        s.context.frame.fbp = 0;
        s.context.frame.fbw = 2; // 128 px
        s.context.frame.psm = GS_PSM_CT32;
        s.context.frame.fbmsk = 0;
        s.context.zbuf.zbp = 1;
        s.context.zbuf.psm = GS_PSM_Z24;
        s.context.zbuf.zmask = 1; // no Z writes: keep it about colour
        s.context.scissor = {0, 0, 127, 63};
        s.context.xyoffset.ofx = 0;
        s.context.xyoffset.ofy = 0;
        s.context.tex0.tfx = 1;
        s.context.tex0.tcc = 0;
        s.prim.type = GS_PRIM_SPRITE;
        s.prim.abe = 0;
        s.prim.tme = 0;
        s.prim.iip = 1;
        s.prim.fst = 1;
        s.colclamp = 1;
        return s;
    }

    PresentationFrame present(GSVkBackend &gs, uint32_t fbp, uint32_t width, uint32_t height)
    {
        GSPresentationRequest req{};
        req.pmode = 1ull; // circuit 1 enabled, circuit 2 off
        // DISPFB1: fbp, width (in 64), 0 offset
        req.dispfb1 = (fbp & 0x1FFu) | (static_cast<uint64_t>(width / 64u & 0x3Fu) << 9);
        // DISPLAY1: w-1 wide (RW), h-1 high (RH), DH/DV divisors 0
        const uint32_t rw = width - 1u, rh = height - 1u;
        req.display1 = (static_cast<uint64_t>(rw) << 32) | (static_cast<uint64_t>(rh) << 44);
        return gs.Present(req);
    }
} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> cases;
    for (int i = 1; i < argc; ++i)
        cases.emplace_back(argv[i]);
    if (cases.empty())
        cases = {"clear", "sprite", "transfer"};

    bool hadDevice = std::getenv("PS2X_VK_ICD") != nullptr;
    if (!hadDevice)
        std::printf("note: PS2X_VK_ICD unset — tests that need a device will fail\n");

    for (const std::string &name : cases)
    {
        std::printf("== case %s\n", name.c_str());
        Ctx c;
        makeCtx(c);
        if (name == "clear")
        {
            GSDrawState s = baseState();
            GSPrimitiveBatch b = spriteBatch(0, 0, 128, 64, 255, 0, 0, s);
            c.gs.Submit(b);
            c.gs.Flush();
            const bool cleared = c.gs.ClearFramebuffer(s.context, 0x4080C0FFu);
            expect(cleared, "ClearFramebuffer accepted the context");
            // a present readback returns the cleared colour in the corner
            PresentationFrame f = present(c.gs, 0, 128, 64);
            expect(static_cast<bool>(f), "present returned a frame");
            if (f)
            {
                // row stride is 640 pixels when the frame is narrower
                const size_t stride = std::max(640u, f.width) * 4u;
                const uint8_t *px = f.pixels.data() + 8 * stride + 8 * 4;
                std::printf("    corner px: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
                expect(px[0] == 0x40 && px[1] == 0x80 && px[2] == 0xC0, "corner pixel is the clear colour");
                expect(px[3] == 0xFF, "alpha forced to 255");
            }
        }
        else if (name == "sprite")
        {
            GSDrawState s = baseState();
            GSPrimitiveBatch b = spriteBatch(16, 8, 48, 40, 0, 255, 0, s);
            c.gs.Submit(b);
            c.gs.Flush();
            PresentationFrame f = present(c.gs, 0, 128, 64);
            expect(static_cast<bool>(f), "present returned a frame");
            if (f)
            {
                const size_t stride = std::max(640u, f.width) * 4u;
                const uint8_t *inside = f.pixels.data() + 24 * stride + 24 * 4;
                const uint8_t *outside = f.pixels.data() + 60 * stride + 100 * 4;
                std::printf("    inside px: %u %u %u %u / outside: %u %u %u %u\n",
                            inside[0], inside[1], inside[2], inside[3], outside[0], outside[1], outside[2], outside[3]);
                expect(inside[1] > 200u && inside[0] < 60u, "inside the sprite is green");
                expect(outside[0] < 40u && outside[1] < 40u && outside[2] < 40u, "outside stays (near) black");
            }
        }
        else if (name == "transfer")
        {
            // upload a 16x2 CT32 image through the transfer path, then draw it textured
            GSDrawState s = baseState();
            s.prim.tme = 1;
            s.prim.fst = 1;
            s.context.tex0.tbp0 = 32 * 32 / 4; // block 256? keep small: block base 256
            s.context.tex0.tbp0 = 256;
            s.context.tex0.tbw = 1;
            s.context.tex0.psm = GS_PSM_CT32;
            s.context.tex0.tfx = 1;
            s.context.tex0.tcc = 1;
            s.textureWidth = 16;
            s.textureHeight = 2;
            // fill vram with a gradient at the image address (0x1000 bytes in)
            for (uint32_t i = 0; i < 32; ++i)
            {
                const uint32_t v = 0xFF000000u | (i * 8u << 16) | (i * 4u << 8) | i;
                std::memcpy(&c.vram[0x1000 + i * 4], &v, 4);
            }
            // simulate the transfer bookkeeping so the texture path reads GS memory
            GSTransferCommand cmd{};
            cmd.direction = 2; // local->local commit (no further Upload() calls follow)
            cmd.bitbltbuf.dpsm = GS_PSM_CT32;
            cmd.bitbltbuf.dbp = 256;
            cmd.bitbltbuf.dbw = 1;
            cmd.trxreg.rrw = 16;
            cmd.trxreg.rrh = 2;
            c.gs.BeginTransfer(cmd);
            c.gs.UploadImage(nullptr, 0);
            GSPrimitiveBatch b = spriteBatch(0, 0, 32, 4, 255, 255, 255, s);
            c.gs.Submit(b);
            c.gs.Flush();
            PresentationFrame f = present(c.gs, 0, 128, 64);
            expect(static_cast<bool>(f), "present returned a frame");
            if (f)
            {
                const size_t stride = std::max(640u, f.width) * 4u;
                const uint8_t *px = f.pixels.data() + 2 * stride + 2 * 4;
                std::printf("    textured px: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
                expect(px[2] > 100u, "texture colour reached the draw (blue channel grows with x)");
            }
        }
        else
        {
            std::printf("  unknown case\n");
            ++failures;
        }
        c.gs.Reset();
    }

    std::printf("== gs_probe %s (%d failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
