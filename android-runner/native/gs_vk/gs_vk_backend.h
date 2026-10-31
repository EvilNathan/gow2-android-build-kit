// Raw Vulkan GS backend (Android) — God of War II (PS2) static-recomp port.
//
// Contract (see gs_vk/README.md): default-constructible GSRasterBackend whose
// every Vulkan call runs on the GS worker thread (GSThreadedBackend drives it).
// Renders the game into RGBA8 render targets keyed by FRAME.FBP at 1x scale,
// mirrors the desktop render-target renderer
// (PS2Recomp/ps2xRuntime/src/lib/gs/gpu/gs_hw_backend.cpp) minus the >1x
// upscale: reinterpreting texture fetch, channel shuffles, destination alpha
// test, Z aliasing, the software blend cases and CLUT-from-target are all in.
//
// Presentation: on Android the backend owns the swapchain created from
// ps2x::host::NativeWindow() (patch 0004: "the Vulkan GS backend presents from
// GS::latchHostPresentationFrame; the host only paces the frame"). Without a
// window (offscreen probe) Present only fills the returned pixels.
#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>

class GSVkBackend final : public GSRasterBackend
{
public:
    GSVkBackend();
    ~GSVkBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
