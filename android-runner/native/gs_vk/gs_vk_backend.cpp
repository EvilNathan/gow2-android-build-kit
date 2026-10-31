// Raw Vulkan GS backend (Android) — see gs_vk_backend.h.
//
// Port of the desktop render-target renderer's CPU side
// (PS2Recomp/ps2xRuntime/src/lib/gs/gpu/gs_hw_backend.cpp) at 1x scale, with
// Vulkan instead of GL. Semantics kept: reinterpreting texture fetch from
// render targets, channel shuffles, destination alpha test, Z aliasing, Z
// inversion, the fixed-function + software blend split, sprite snapping and
// the CLUT-from-target path (on the CPU here: the target's pixels are read
// back and the reinterpreted words are built with the GS swizzle tables).
#include "gs_vk_backend.h"

#include "gs_vk_loader.h"

#include "gpu/gs_texture_store.h"
#include "runtime/gs/gs_swizzle.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/gs_backend.h"
#include "gs_gpu_helpers.h"

#include "gs_vk_shaders_generated.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef __ANDROID__
#include "ps2x_host.h" // ps2x::host::NativeWindow(): the ANativeWindow to present to
#else
// Desktop (offscreen probe): no window, Present only reads pixels back.
namespace ps2x::host
{
    inline void *NativeWindow() { return nullptr; }
}
#endif

namespace
{
    constexpr uint32_t kRtRows = 512u; // render target height in PS2 pixels (like the desktop renderer)
    constexpr float kScale = 1.0f;     // 1x: every PS2 pixel is one host pixel

    // ---- std140 blocks (must match shaders/*.vert|frag) ------------------------------------------

    struct DrawUbo
    {
        float uInvSize[2];      // 0
        float uTexSize[2];      // 8
        float uTexDim[2];       // 16
        float uRtClamp[2];      // 24
        float uPxScale;         // 32
        float uZAliasMax;       // 36
        int32_t uReinterp;      // 40
        int32_t uDate;          // 44
        int32_t uDatm;          // 48
        int32_t uSwBlend;       // 52
        int32_t uColClamp;      // 56
        int32_t pad0;           // 60 (ivec4 uAbcd aligns to 16)
        int32_t uAbcd[4];       // 64
        float uFix;             // 80
        int32_t uZAlias;        // 84
        int32_t uZAliasWrite;   // 88
        int32_t uZTex;          // 92
        float uZMax;            // 96
        int32_t uIndexed;       // 100
        int32_t uLinear;        // 104
        int32_t pad1;           // 108 (ivec4 uWrap aligns to 16)
        int32_t uWrap[4];       // 112
        int32_t uTme;           // 128
        int32_t uFst;           // 132
        int32_t uTfx;           // 136
        int32_t uTcc;           // 140
        int32_t uIip;           // 144
        int32_t uAniso;         // 148
        int32_t uRtTex24;       // 152
        float uTa0;             // 156
        float uTa1;             // 160
        int32_t uAem;           // 164
        int32_t uAte;           // 168
        int32_t uAtst;          // 172
        int32_t uAref;          // 176
        int32_t uFailTint;      // 180
        int32_t uPass;          // 184
        float uBlendScale;      // 188
        int32_t uTexFmt[4];     // 192
        int32_t uTexFmt2[4];    // 208
        int32_t uRtFmt[4];      // 224
        int32_t uRtFmt2[4];     // 240
        int32_t uTex0[4];       // 256
        float uRtPx[2];         // 272
        int32_t uPad0;          // 280
        int32_t uPad1;          // 284
    };
    static_assert(sizeof(DrawUbo) == 288, "DrawUbo must match the std140 block in shaders/draw.*");

    struct ShuffleUbo
    {
        float uInvSize[2];  // 0
        float uZMax;        // 8
        int32_t uSrcZ;      // 12
        int32_t uHs;        // 16
        int32_t uHd;        // 20
        float uScale;       // 24
        int32_t pad0;       // 28
        int32_t uTexa[4];   // 32
        float uPad2[2];     // 48
    };
    static_assert(sizeof(ShuffleUbo) == 56, "ShuffleUbo must match the std140 block in shaders/shuffle.*");

    struct PresentUbo
    {
        int32_t uSrcOffset[4];
        int32_t uSrcEdge[4];
    };

    // GS vertex as sent to the GPU (same layout as the desktop HwVertex).
    struct HwVertex
    {
        float x, y, z;
        float r, g, b, a;
        float s, t, q;
        float u, v;
        float cu0 = -1e30f, cv0 = -1e30f, cu1 = 1e30f, cv1 = 1e30f;
    };

    struct ShuffleVertex
    {
        float x, y, ox, oy;
    };

    // ---- small helpers -----------------------------------------------------------------------------

    uint32_t pageRowsFor(uint32_t psm)
    {
        switch (psm & 0x3F)
        {
        case 0x13: case 0x1B: case 0x24: case 0x2C: return 64u;
        case 0x14: case 0x2C + 1: return 128u;
        case 0x02: case 0x0A: case 0x32: case 0x3A: return 64u;
        default: return 32u;
        }
    }

    uint32_t spanBlocks(uint32_t psm, uint32_t bw, uint32_t rows)
    {
        const uint32_t pagesDown = (rows + pageRowsFor(psm) - 1u) / pageRowsFor(psm);
        return std::max<uint32_t>(1u, pagesDown) * std::max<uint32_t>(1u, bw) * 32u;
    }

    bool family16(uint8_t psm) { return psm == GS_PSM_CT16 || psm == GS_PSM_CT16S; }
    bool isIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }
    float zMaxFor(uint8_t zpsm)
    {
        switch (zpsm)
        {
        case 0x30: return 4294967295.0f;
        case 0x32: case 0x3A: return 65535.0f;
        default: return 16777215.0f;
        }
    }
    uint32_t fbmsk16(uint32_t m)
    {
        return ((m >> 3) & 0x1Fu) | ((m >> 6) & 0x3E0u) | ((m >> 9) & 0x7C00u) | ((m >> 16) & 0x8000u);
    }

    // Bump-allocated persistently mapped host buffer (vertices, UBOs, staging).
    struct GpuRing
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t *mapped = nullptr;
        VkDeviceSize capacity = 0;
        VkDeviceSize offset = 0;

        void create(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize bytes, uint32_t memTypes);
        void destroy(VkDevice dev);
        struct Alloc
        {
            VkDeviceSize offset;
            void *cpu;
        };
        // Returns nullptr when `bytes` does not fit (caller flushes and retries).
        Alloc alloc(VkDeviceSize bytes, VkDeviceSize align);
        void reset() { offset = 0; }
    };

    void GpuRing::create(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize bytes, uint32_t memTypes)
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        gs_vk::vk.CreateBuffer(dev, &bi, nullptr, &buffer);
        VkMemoryRequirements req{};
        gs_vk::vk.GetBufferMemoryRequirements(dev, buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memTypes; // host visible type pre-selected by the caller
        gs_vk::vk.AllocateMemory(dev, &ai, nullptr, &memory);
        gs_vk::vk.BindBufferMemory(dev, buffer, memory, 0);
        gs_vk::vk.MapMemory(dev, memory, 0, req.size, 0, reinterpret_cast<void **>(&mapped));
        capacity = req.size;
        offset = 0;
    }

    void GpuRing::destroy(VkDevice dev)
    {
        if (buffer) gs_vk::vk.DestroyBuffer(dev, buffer, nullptr);
        if (memory) gs_vk::vk.FreeMemory(dev, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        mapped = nullptr;
        capacity = 0;
        offset = 0;
    }

    GpuRing::Alloc GpuRing::alloc(VkDeviceSize bytes, VkDeviceSize align)
    {
        VkDeviceSize at = (offset + align - 1) & ~(align - 1);
        if (at + bytes > capacity)
            return {0, nullptr};
        offset = at + bytes;
        return {at, mapped + at};
    }
} // namespace

struct GSVkBackend::Impl
{
    // ---- Vulkan objects ---------------------------------------------------------------------------
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer frameCb = VK_NULL_HANDLE;   // draws + uploads of the current frame
    bool cbOpen = false;
    VkFence frameFence = VK_NULL_HANDLE;

    uint32_t hostMemType = 0;    // HOST_VISIBLE | HOST_COHERENT
    uint32_t deviceMemType = 0;  // DEVICE_LOCAL (falls back to hostMemType on SwiftShader)

    GpuRing rings;          // vertices + UBOs
    GpuRing staging;        // texture upload staging

    // descriptor layout: 0 uTex, 1 uDest(input), 2 uTables, 3 uRtColor, 4 uPalette, 5 uIdxPalette, 6 UBO
    VkDescriptorSetLayout drawSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkPipelineLayout drawPipelineLayout = VK_NULL_HANDLE;

    VkRenderPass rpDraw = VK_NULL_HANDLE;      // color LOAD + depth LOAD (+ input attachment)
    VkRenderPass rpClearColorOnly = VK_NULL_HANDLE; // color CLEAR, single attachment
    VkRenderPass rpClearColor = VK_NULL_HANDLE; // color CLEAR + depth LOAD
    VkRenderPass rpInitCD = VK_NULL_HANDLE;    // color CLEAR + depth CLEAR (target creation)
    VkRenderPass rpPresent = VK_NULL_HANDLE;   // present image, color CLEAR

    enum class Program
    {
        Draw,
        Shuffle,
        Present,
    };
    VkShaderModule vsDraw = VK_NULL_HANDLE, fsDraw = VK_NULL_HANDLE;
    VkShaderModule vsShuffle = VK_NULL_HANDLE, fsShuffle = VK_NULL_HANDLE;
    VkShaderModule vsPresent = VK_NULL_HANDLE, fsPresent = VK_NULL_HANDLE;

    struct PipelineKey
    {
        Program program;
        uint32_t blendEnable;
        uint32_t srcColor, dstColor, srcAlpha, dstAlpha;
        uint32_t equation;
        uint32_t depthTest;
        uint32_t depthWrite;
        uint32_t depthCompare;
        uint32_t writeMask;
        bool operator==(const PipelineKey &o) const
        {
            return std::memcmp(this, &o, sizeof(*this)) == 0;
        }
    };
    struct PipelineKeyHash
    {
        size_t operator()(const PipelineKey &k) const
        {
            uint64_t h = 1469598103934665603ull;
            const auto mix = [&h](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
            const uint32_t *p = reinterpret_cast<const uint32_t *>(&k);
            for (size_t i = 0; i < sizeof(k) / 4; ++i)
                mix(p[i]);
            return static_cast<size_t>(h);
        }
    };
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;

    VkImage tableImage = VK_NULL_HANDLE;
    VkDeviceMemory tableMem = VK_NULL_HANDLE;
    VkImageView tableView = VK_NULL_HANDLE;
    VkSampler nearestSampler = VK_NULL_HANDLE, linearSampler = VK_NULL_HANDLE;

    struct DummyImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    DummyImage whiteTex;   // 1x1 white: bound to uTex for untextured draws
    DummyImage blackTex;   // 1x1 black: bound to unused reinterpret/palette bindings
    DummyImage paletteTex; // 256x1: updated per reinterpret draw / indexed draw

    // ---- render targets ---------------------------------------------------------------------------
    struct RenderTarget
    {
        VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
        VkDeviceMemory colorMem = VK_NULL_HANDLE, depthMem = VK_NULL_HANDLE;
        VkImageView colorAttach = VK_NULL_HANDLE, colorSampled = VK_NULL_HANDLE, colorInput = VK_NULL_HANDLE;
        VkImageView depthAttach = VK_NULL_HANDLE, depthSampled = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
        VkFramebuffer fbColorOnly = VK_NULL_HANDLE;
        uint32_t fbp = 0, fbw = 1; // PS2 page base and width in 64 px units
        uint8_t psm = 0;
        uint32_t zbp = ~0u;
        uint32_t pxW = 0, pxH = kRtRows;
        uint64_t version = 1, lastDrawSeq = 0, lastDepthSeq = 0;
        uint32_t usedW = 0, usedH = 0;
        std::vector<uint8_t> cpu;      // RGBA8 readback for CLUT-from-target
        uint64_t cpuVersion = 0;
        bool depthCleared = false;
    };
    std::vector<RenderTarget> targets;

    // ---- textures ---------------------------------------------------------------------------------
    struct Texture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        uint32_t blockLo = 0, blockHi = 0;
        bool valid = false;
        uint64_t lastUse = 0;
    };
    std::unordered_map<uint64_t, Texture> textures;
    std::unordered_map<uint64_t, Texture> palettes; // palettes reuse the Texture struct (256x1)
    uint64_t useCounter = 0, writeSeq = 0;
    struct UploadRecord
    {
        uint32_t lo, hi;
        uint64_t seq;
    };
    std::vector<UploadRecord> uploadLog;
    uint64_t clutHash = 0;

    // forward page tables + inverse tables in one R16UI texture (2048 entries per row)
    std::unordered_map<const uint16_t *, uint32_t> tableOffset;
    std::unordered_map<uint32_t, uint32_t> inverseOffset;
    std::unordered_map<uint32_t, std::vector<uint16_t>> ownerInverse;

    // ---- GS state ---------------------------------------------------------------------------------
    GSTextureStore store;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;
    bool initialized = false, failed = false, dualSrc = false;

    std::vector<HwVertex> pending;
    GSDrawState pendingState{};
    bool havePending = false;

    struct LastSprite
    {
        float xs[2], ys[2], us[2], vs[2];
        size_t end = 0;
        uint64_t batch = ~0ull;
    } lastSprite;
    uint64_t batchSerial = 0;

    std::vector<ShuffleVertex> shuffleQuads[4];
    GSDrawState shuffleState{};
    bool haveShuffle = false;
    RenderTarget *shuffleDst = nullptr, *shuffleSrc = nullptr;
    bool shuffleSrcZ = false;

    std::vector<uint32_t> invertedZ;
    int presentCount = 0;
    uint64_t clutCacheSeq = 0;

    // swapchain (Android)
    struct Swap
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> images;
        VkSemaphore acquired = VK_NULL_HANDLE;  // binary, image acquired
        VkSemaphore rendered = VK_NULL_HANDLE;  // binary, blit done
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool ok = false;
    } swap;
    void *presentWindow = nullptr;

    // present image + readback
    VkImage presentImage = VK_NULL_HANDLE;
    VkDeviceMemory presentMem = VK_NULL_HANDLE;
    VkImageView presentView = VK_NULL_HANDLE;
    VkFramebuffer presentFb = VK_NULL_HANDLE;
    uint32_t presentW = 0, presentH = 0;
    VkBuffer readbackBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readbackMem = VK_NULL_HANDLE;
    uint8_t *readbackMapped = nullptr;
    uint32_t readbackCapacity = 0;

    bool ensure();
    void fail(const char *what);
    bool createDeviceObjects();
    void destroyDeviceObjects();
    uint32_t memoryType(VkMemoryRequirements req, VkMemoryPropertyFlags props) const;

    // command buffer helpers
    void beginFrameCb();
    void endFrameCb();
    void submitAndWait();
    void globalBarrier();

    VkPipeline pipeline(const PipelineKey &key);
    VkPipeline buildPipeline(const PipelineKey &key);
    VkShaderModule shader(const char *name);

    // render targets
    RenderTarget *findTarget(uint32_t fbp);
    RenderTarget &target(uint32_t fbp, uint32_t fbw);
    void createTarget(RenderTarget &t, uint32_t fbp, uint32_t fbw);
    void destroyTarget(RenderTarget &t);

    // textures
    Texture &uploadTexture(uint64_t key, uint32_t w, uint32_t h, const void *rgba, uint32_t pitchBytes, bool singleByte,
                           uint32_t blockLo, uint32_t blockHi);
    void decodeDirect(const GSDrawState &s, std::vector<uint32_t> &out);
    Texture &textureFor(const GSDrawState &s);
    Texture &indexTextureFor(const GSDrawState &s);
    Texture &paletteFor(const GSDrawState &s);
    void evictOld();
    void invalidateBlocks(uint32_t lo, uint32_t hi);
    void ensurePaletteImage();
    void uploadPaletteImage(const GSDrawState &s);

    // CLUT from a render target (CPU: read the target back, rebuild the raw words)
    bool clutFromTarget(const GSTex0Reg &tex0, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words);
    bool readTargetCpu(RenderTarget &rt);

    // drawing
    void submit(const GSPrimitiveBatch &b);
    void addVertex(const GSDrawState &s, const GSVertex &v, float u, float tv, float zmax);
    void draw(const GSDrawState &s, const std::vector<HwVertex> &verts);
    void flushPending();
    void blendState(const GSDrawState &s, bool &enabled, uint32_t &sf, uint32_t &df, float &constant, uint32_t &equation, bool &soft);
    static bool sameState(const GSDrawState &a, const GSDrawState &b) { return std::memcmp(&a, &b, sizeof(GSDrawState)) == 0; }
    bool isZInversion(const GSDrawState &s, const std::vector<HwVertex> &verts);
    bool zInverted(uint32_t zbp) const { return std::find(invertedZ.begin(), invertedZ.end(), zbp) != invertedZ.end(); }
    bool depthClearSprite(const GSDrawState &s, const GSPrimitiveBatch &b);
    static bool depthRowOf(const RenderTarget &t, uint32_t page, uint32_t fbw, uint8_t zpsm, int &rowOffset);

    // channel shuffles
    const std::vector<uint16_t> &inverseFor(uint32_t ownerPsm);
    bool viewToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm, uint32_t fbp, uint32_t fbw,
                     uint32_t &ox, uint32_t &oy, uint32_t &half);
    bool unitToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm, uint32_t fbp, uint32_t fbw,
                     uint32_t &ox, uint32_t &oy, uint32_t &half);
    bool tryShuffle(const GSDrawState &s, const GSPrimitiveBatch &b);
    void flushShuffle();

    // presentation
    void ensurePresentImage(uint32_t w, uint32_t h);
    void ensureSwapchain();
    void destroySwapchain();
    void drawCircuit(VkImageView src, uint32_t cdbx, uint32_t cdby, bool ct24, bool alphaFactor, uint32_t srcEdge, bool blendOver,
                     uint32_t pmode, bool mmod, uint32_t alp);

    void trash(void *obj, bool isMemory);
    std::vector<std::pair<void *, bool>> trash_;
    void emptyTrash();
};

// ==== lifecycle ====================================================================================

GSVkBackend::GSVkBackend() : m_impl(std::make_unique<Impl>()) {}
GSVkBackend::~GSVkBackend() = default;

void GSVkBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_impl->vram = vram;
    m_impl->vramSize = vramSize;
    m_impl->store.Initialize(vram, vramSize);
    m_impl->ensure(); // like the desktop renderer: context created at Initialize
}

void GSVkBackend::Reset()
{
    Impl &d = *m_impl;
    if (d.failed)
        return;
    d.flushPending();
    d.store.Reset();
    d.invertedZ.clear();
    for (auto &kv : d.textures)
        kv.second.valid = false;
    for (auto &kv : d.palettes)
        kv.second.valid = false;
}

void GSVkBackend::Submit(const GSPrimitiveBatch &batch)
{
    Impl &d = *m_impl;
    if (batch.vertexCount == 0u || d.failed)
        return;
    if (!d.ensure())
        return;
    d.submit(batch);
}

void GSVkBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Impl &d = *m_impl;
    if (d.failed)
        return;
    d.flushPending();
    if (d.initialized)
        d.store.LoadClut(tex0, texclut, [&d](const GSTex0Reg &t, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words)
                         { return d.clutFromTarget(t, width, xy, count, words); });
    else
        d.store.LoadClut(tex0, texclut);
    d.clutHash = d.store.ClutHash();
}

void GSVkBackend::BeginTransfer(const GSTransferCommand &command)
{
    Impl &d = *m_impl;
    if (d.failed)
        return;
    d.flushPending();
    d.store.BeginTransfer(command);
    if ((command.direction == 0u || command.direction == 2u) && d.vram)
    {
        const uint32_t rows = static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh;
        const uint32_t lo = command.bitbltbuf.dbp, hi = lo + spanBlocks(command.bitbltbuf.dpsm, command.bitbltbuf.dbw, rows);
        d.invalidateBlocks(lo, hi);
        if (d.uploadLog.size() > 4096u)
            d.uploadLog.erase(d.uploadLog.begin(), d.uploadLog.begin() + 2048);
        d.uploadLog.push_back({lo, hi, ++d.writeSeq});
    }
}

void GSVkBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Impl &d = *m_impl;
    if (d.failed)
        return;
    d.store.Upload(data, sizeBytes);
}

void GSVkBackend::Flush()
{
    if (!m_impl->failed)
        m_impl->flushPending();
}

void GSVkBackend::TextureFlush()
{
    if (!m_impl->failed)
        m_impl->flushPending();
}

void GSVkBackend::Sync(GSSyncReason)
{
    if (!m_impl->failed)
        m_impl->flushPending();
}

bool GSVkBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    Impl &d = *m_impl;
    if (d.failed || !d.ensure() || context.frame.fbw == 0u)
        return false;
    d.flushPending();
    GSVkBackend::Impl::RenderTarget &rt = d.target(context.frame.fbp, context.frame.fbw);
    std::fprintf(stderr, "[gs-vk] ClearFramebuffer: rt %ux%u\n", rt.pxW, rt.pxH);
    d.beginFrameCb();
    // colour-only render pass with a LOAD_OP_CLEAR (no depth attachment, no input attachment):
    // the narrowest path a software rasterizer is guaranteed to take
    VkClearValue cv{};
    cv.color = {{(rgba & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = d.rpClearColorOnly;
    rb.framebuffer = rt.fbColorOnly;
    rb.renderArea.extent = {rt.pxW, rt.pxH};
    rb.pClearValues = &cv;
    rb.clearValueCount = 1;
    gs_vk::vk.CmdBeginRenderPass(d.frameCb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    gs_vk::vk.CmdEndRenderPass(d.frameCb);
    d.endFrameCb();
    d.submitAndWait();
    ++rt.version;
    return true;
}

uint32_t GSVkBackend::ConsumeLocalToHostBytes(uint8_t *, uint32_t)
{
    return 0u; // the game never reads GS memory back
}

uint32_t GSVkBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    return m_impl->store.Read(psm, base, bw, x, y);
}

void GSVkBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m_impl->store.Write(psm, base, bw, x, y, value);
}

void GSVkBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    m_impl->store.Snapshot(out);
}

GSTransferSnapshot GSVkBackend::GetTransferSnapshot() const
{
    return m_impl->store.TransferState();
}

// ==== Vulkan context ===============================================================================

namespace
{
    VKAPI_ATTR VkBool32 VKAPI_CALL vkDebugCb(VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT, uint64_t, size_t, int32_t, const char *, const char *msg)
    {
        std::fprintf(stderr, "[gs-vk] %s\n", msg);
        return VK_FALSE;
    }
}

void GSVkBackend::Impl::fail(const char *what)
{
    std::fprintf(stderr, "[gs-vk] unavailable: %s\n", what);
    failed = true;
}

bool GSVkBackend::Impl::ensure()
{
    if (initialized)
        return true;
    if (failed)
        return false;

    if (!gs_vk::Load())
        return fail("no Vulkan loader (libvulkan.so / vk_swiftshader.dll)"), false;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gow2-android";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    const char *exts[4];
    uint32_t extCount = 0;
    (void)exts;
    (void)extCount;
#ifdef __ANDROID__
    const char *want[2] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    // SwiftShader-style ICDs (desktop probe path) have no surface extensions;
    // the Android loader always has these.
    ii.enabledExtensionCount = 2;
    ii.ppEnabledExtensionNames = want;
#endif
    if (gs_vk::vk.CreateInstance(&ii, nullptr, &instance) != VK_SUCCESS)
        return fail("vkCreateInstance"), false;
    if (!gs_vk::ResolveInstanceFunctions(instance))
        return fail("instance entry points"), false;

    uint32_t ndev = 0;
    gs_vk::vk.EnumeratePhysicalDevices(instance, &ndev, nullptr);
    if (ndev == 0)
        return fail("no Vulkan physical device"), false;
    std::vector<VkPhysicalDevice> devs(ndev);
    gs_vk::vk.EnumeratePhysicalDevices(instance, &ndev, devs.data());
    phys = devs[0];
    VkPhysicalDeviceProperties props{};
    gs_vk::vk.GetPhysicalDeviceProperties(phys, &props);
    for (VkPhysicalDevice p : devs)
    {
        VkPhysicalDeviceProperties pp{};
        gs_vk::vk.GetPhysicalDeviceProperties(p, &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        {
            phys = p;
            props = pp;
            break;
        }
    }
    std::fprintf(stderr, "[gs-vk] device: %s (%s)\n", props.deviceName,
                 props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "software" : "hardware");

    uint32_t nq = 0;
    gs_vk::vk.GetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    gs_vk::vk.GetPhysicalDeviceQueueFamilyProperties(phys, &nq, qs.data());
    bool found = false;
    for (uint32_t i = 0; i < nq; ++i)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            queueFamily = i;
            found = true;
            break;
        }
    if (!found)
        return fail("no graphics queue"), false;

    VkPhysicalDeviceFeatures features{};
    gs_vk::vk.GetPhysicalDeviceFeatures(phys, &features);
    dualSrc = features.dualSrcBlend != 0;
    if (!dualSrc)
        std::fprintf(stderr, "[gs-vk] dual-source blending unavailable: the software blend path covers all draws\n");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex = queueFamily;
    qc.queueCount = 1;
    qc.pQueuePriorities = &prio;
    // VK_KHR_swapchain drives the Android surface; the desktop probe has no
    // loader ICD registry but SwiftShader still exports its functions once the
    // extension is enabled.
    const char *deviceExts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qc;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = deviceExts;
    di.pEnabledFeatures = &features;
    if (gs_vk::vk.CreateDevice(phys, &di, nullptr, &device) != VK_SUCCESS)
        return fail("vkCreateDevice"), false;
    if (!gs_vk::ResolveDeviceFunctions(device))
        return fail("missing device entry points"), false;

    gs_vk::vk.GetDeviceQueue(device, queueFamily, 0, &queue);

    VkPhysicalDeviceMemoryProperties memProps{};
    gs_vk::vk.GetPhysicalDeviceMemoryProperties(phys, &memProps);
    bool gotHost = false, gotDevice = false;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        const VkMemoryPropertyFlags f = memProps.memoryTypes[i].propertyFlags;
        if (!gotHost && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            hostMemType = i, gotHost = true;
        if (!gotDevice && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            deviceMemType = i, gotDevice = true;
    }
    if (!gotDevice)
        deviceMemType = hostMemType;

    frameFence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    gs_vk::vk.CreateFence(device, &fi, nullptr, &frameFence);
    swap.acquired = swap.rendered = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    gs_vk::vk.CreateSemaphoreF(device, &si, nullptr, &swap.acquired);
    gs_vk::vk.CreateSemaphoreF(device, &si, nullptr, &swap.rendered);
    rings.create(device, phys, 48u * 1024u * 1024u, hostMemType);
    staging.create(device, phys, 48u * 1024u * 1024u, hostMemType);

    if (!createDeviceObjects())
        return false;

    if (!rings.mapped || !staging.mapped)
        return fail("ring allocation"), false;

    

    std::fprintf(stderr, "[gs-vk] raw Vulkan GS backend ready (dualSrc=%d)\n", dualSrc ? 1 : 0);
    initialized = true;
    return true;
}

bool GSVkBackend::Impl::createDeviceObjects()
{
    // command pool
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = queueFamily;
    if (gs_vk::vk.CreateCommandPool(device, &ci, nullptr, &cmdPool) != VK_SUCCESS)
        return fail("command pool"), false;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = cmdPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    gs_vk::vk.AllocateCommandBuffers(device, &ca, &frameCb);

    // descriptor layout
    VkDescriptorSetLayoutBinding b[7]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[6] = {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 7;
    li.pBindings = b;
    if (gs_vk::vk.CreateDescriptorSetLayout(device, &li, nullptr, &drawSetLayout) != VK_SUCCESS)
        return fail("descriptor layout"), false;

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &drawSetLayout;
    if (gs_vk::vk.CreatePipelineLayout(device, &pli, nullptr, &drawPipelineLayout) != VK_SUCCESS)
        return fail("pipeline layout"), false;

    // descriptor pool
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 * 5},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 8192},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 8192;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (gs_vk::vk.CreateDescriptorPool(device, &pi, nullptr, &descPool) != VK_SUCCESS)
        return fail("descriptor pool"), false;

    // shaders
    vsDraw = shader("draw.vert");
    fsDraw = shader("draw.frag");
    vsShuffle = shader("shuffle.vert");
    fsShuffle = shader("shuffle.frag");
    vsPresent = shader("present.vert");
    fsPresent = shader("present.frag");
    if (!vsDraw || !fsDraw || !vsShuffle || !fsShuffle || !vsPresent || !fsPresent)
        return fail("embedded shaders missing"), false;

    // render passes: all images in GENERAL layout; the draw pass reads its own
    // colour attachment as an input attachment (destination alpha test / soft
    // blend / Z alias), so it carries a by-region self dependency.
    VkAttachmentDescription attColor{0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                     VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                     VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentDescription attDepth{0, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                                     VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                     VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference cRef{0, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference dRef{1, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cRef;
    sub.pDepthStencilAttachment = &dRef;
    sub.inputAttachmentCount = 0;
    sub.pInputAttachments = nullptr; // EXPERIMENT
    VkSubpassDependency self{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_ACCESS_MEMORY_WRITE_BIT,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                             0};
    VkSubpassDependency ext{VK_SUBPASS_EXTERNAL, 0,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0};
    VkSubpassDependency extOut{0, VK_SUBPASS_EXTERNAL,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0};
    VkSubpassDependency deps[2] = {self, ext};
    {
        VkAttachmentDescription atts[2] = {attColor, attDepth};
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 2;
        ri.pAttachments = atts;
        ri.subpassCount = 1;
        ri.pSubpasses = &sub;
        ri.dependencyCount = 2;
        ri.pDependencies = deps;
        if (gs_vk::vk.CreateRenderPass(device, &ri, nullptr, &rpDraw) != VK_SUCCESS)
            return fail("draw render pass"), false;
    }
    {
        VkAttachmentDescription atts[2] = {attColor, attDepth};
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 2;
        ri.pAttachments = atts;
        ri.subpassCount = 1;
        ri.pSubpasses = &sub;
        ri.dependencyCount = 2;
        ri.pDependencies = deps;
        if (gs_vk::vk.CreateRenderPass(device, &ri, nullptr, &rpClearColor) != VK_SUCCESS)
            return fail("clear render pass"), false;
    }
    {
        // single colour attachment, LOAD_OP_CLEAR, no depth, no input
        VkAttachmentDescription atts[1] = {attColor};
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkSubpassDescription csub{};
        csub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        csub.colorAttachmentCount = 1;
        csub.pColorAttachments = &cRef;
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 1;
        ri.pAttachments = atts;
        ri.subpassCount = 1;
        ri.pSubpasses = &csub;
        ri.dependencyCount = 2;
        VkSubpassDependency pdeps[2] = {ext, extOut};
        ri.pDependencies = pdeps;
        if (gs_vk::vk.CreateRenderPass(device, &ri, nullptr, &rpClearColorOnly) != VK_SUCCESS)
            return fail("clear-only render pass"), false;
    }
    {
        VkAttachmentDescription atts[2] = {attColor, attDepth};
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 2;
        ri.pAttachments = atts;
        ri.subpassCount = 1;
        ri.pSubpasses = &sub;
        ri.dependencyCount = 2;
        ri.pDependencies = deps;
        if (gs_vk::vk.CreateRenderPass(device, &ri, nullptr, &rpInitCD) != VK_SUCCESS)
            return fail("init render pass"), false;
    }
    {
        // present image: one colour attachment, also bound as input attachment
        // (descriptor set layout is shared with the draw program), no depth
        VkAttachmentDescription atts[1] = {attColor};
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkSubpassDescription psub{};
        psub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        psub.colorAttachmentCount = 1;
        psub.pColorAttachments = &cRef;
        psub.inputAttachmentCount = 1;
        psub.pInputAttachments = &cRef;
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 1;
        ri.pAttachments = atts;
        ri.subpassCount = 1;
        ri.pSubpasses = &psub;
        ri.dependencyCount = 2;
        VkSubpassDependency pdeps[2] = {ext, extOut};
        ri.pDependencies = pdeps;
        if (gs_vk::vk.CreateRenderPass(device, &ri, nullptr, &rpPresent) != VK_SUCCESS)
            return fail("present render pass"), false;
    }

    // samplers
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 16.0f;
    if (gs_vk::vk.CreateSampler(device, &sci, nullptr, &nearestSampler) != VK_SUCCESS)
        return fail("nearest sampler"), false;
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    if (gs_vk::vk.CreateSampler(device, &sci, nullptr, &linearSampler) != VK_SUCCESS)
        return fail("linear sampler"), false;

    // white/black 1x1 + 256x1 palette dummy
    {
    const uint32_t white = 0xFFFFFFFFu, black = 0u;
        auto make = [&](DummyImage &img, uint32_t w, uint32_t h, uint32_t pixel) {
            VkImageCreateInfo imi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imi.imageType = VK_IMAGE_TYPE_2D;
            imi.format = VK_FORMAT_R8G8B8A8_UNORM;
            imi.extent = {w, h, 1};
            imi.mipLevels = imi.arrayLayers = 1;
            imi.samples = VK_SAMPLE_COUNT_1_BIT;
            imi.tiling = VK_IMAGE_TILING_OPTIMAL;
            imi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (gs_vk::vk.CreateImage(device, &imi, nullptr, &img.image) != VK_SUCCESS)
                return false;
            VkMemoryRequirements req{};
            gs_vk::vk.GetImageMemoryRequirements(device, img.image, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memoryType(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (gs_vk::vk.AllocateMemory(device, &ai, nullptr, &img.mem) != VK_SUCCESS)
                return false;
            gs_vk::vk.BindImageMemory(device, img.image, img.mem, 0);
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (gs_vk::vk.CreateImageView(device, &vi, nullptr, &img.view) != VK_SUCCESS)
                return false;
            // upload via a one-shot command buffer
            GpuRing tmp;
            tmp.create(device, phys, 65536, hostMemType);
            auto a = tmp.alloc(4, 4);
            std::memcpy(a.cpu, &pixel, 4);
            VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ca.commandPool = cmdPool;
            ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ca.commandBufferCount = 1;
            VkCommandBuffer cb;
            gs_vk::vk.AllocateCommandBuffers(device, &ca, &cb);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            gs_vk::vk.BeginCommandBuffer(cb, &bi);
            VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.image = img.image;
            bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcAccessMask = 0;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            gs_vk::vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
            VkBufferImageCopy cp{};
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.imageExtent = {w, h, 1};
            gs_vk::vk.CmdCopyBufferToImage(cb, tmp.buffer, img.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, &cp);
            gs_vk::vk.EndCommandBuffer(cb);
            VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            su.commandBufferCount = 1;
            su.pCommandBuffers = &cb;
            gs_vk::vk.QueueSubmit(queue, 1, &su, VK_NULL_HANDLE);
            gs_vk::vk.QueueWaitIdle(queue);
            gs_vk::vk.FreeCommandBuffers(device, cmdPool, 1, &cb);
            tmp.destroy(device);
            return true;
        };
        if (!make(whiteTex, 1, 1, white) || !make(blackTex, 1, 1, black))
            return fail("dummy textures"), false;
        // palette dummy: 256 black entries; reinterpreted draws upload the real one
        std::vector<uint32_t> pal(256, 0u);
        if (!make(paletteTex, 256, 1, black))
            return fail("palette texture"), false;
        (void)pal;
    }

    // page tables (uTables): forward tables of the texture formats + inverse
    // tables of the render target formats, one R16UI texture of 2048-wide rows.
    {
        std::vector<uint16_t> data;
        const uint32_t psms[] = {0x00, 0x01, 0x02, 0x0A, 0x13, 0x14, 0x1B, 0x24, 0x2C};
        for (const uint32_t psm : psms)
        {
            const GSSwizzle::Format &f = GSSwizzle::GetFormat(psm);
            if (!f.table || tableOffset.count(f.table))
                continue;
            const uint32_t size = 32u << (f.pageShiftX + f.pageShiftY);
            tableOffset[f.table] = static_cast<uint32_t>(data.size());
            data.insert(data.end(), f.table, f.table + size);
        }
        for (const uint32_t psm : {0x00u, 0x01u, 0x02u, 0x0Au})
        {
            const GSSwizzle::Format &f = GSSwizzle::GetFormat(psm);
            const uint32_t pageSize = 1u << (f.pageShiftX + f.pageShiftY);
            inverseOffset[psm] = static_cast<uint32_t>(data.size());
            data.resize(data.size() + pageSize, 0);
            uint16_t *inv = &data[inverseOffset[psm]];
            for (uint32_t e = 0; e < pageSize; ++e)
                inv[f.table[e]] = static_cast<uint16_t>(e);
        }
        const uint32_t rows = static_cast<uint32_t>((data.size() + 2047u) / 2048u);
        data.resize(static_cast<size_t>(rows) * 2048u, 0);

        VkImageCreateInfo imi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imi.imageType = VK_IMAGE_TYPE_2D;
imi.format = VK_FORMAT_R16_UINT;
        imi.extent = {2048, rows, 1};
        imi.mipLevels = imi.arrayLayers = 1;
        imi.samples = VK_SAMPLE_COUNT_1_BIT;
        imi.tiling = VK_IMAGE_TILING_OPTIMAL;
        imi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        gs_vk::vk.CreateImage(device, &imi, nullptr, &tableImage);
        VkMemoryRequirements req{};
        gs_vk::vk.GetImageMemoryRequirements(device, tableImage, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        gs_vk::vk.AllocateMemory(device, &ai, nullptr, &tableMem);
        gs_vk::vk.BindImageMemory(device, tableImage, tableMem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
vi.image = tableImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16_UINT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        gs_vk::vk.CreateImageView(device, &vi, nullptr, &tableView);

        beginFrameCb();
auto a = staging.alloc(data.size() * 2, 4);
std::memcpy(a.cpu, data.data(), data.size() * 2);
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.image = tableImage;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        gs_vk::vk.CmdPipelineBarrier(frameCb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {2048, rows, 1};
        gs_vk::vk.CmdCopyBufferToImage(frameCb, staging.buffer, tableImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        VkImageMemoryBarrier bar2 = bar;
        bar2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        gs_vk::vk.CmdPipelineBarrier(frameCb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar2);
        endFrameCb();
        submitAndWait();
    }

    // present readback buffer (a 4032-px-wide target at 512 rows is the worst case)
    {
                readbackCapacity = 4096u * 512u * 4u;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = readbackCapacity;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        gs_vk::vk.CreateBuffer(device, &bi, nullptr, &readbackBuffer);
        VkMemoryRequirements req{};
        gs_vk::vk.GetBufferMemoryRequirements(device, readbackBuffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        gs_vk::vk.AllocateMemory(device, &ai, nullptr, &readbackMem);
        gs_vk::vk.BindBufferMemory(device, readbackBuffer, readbackMem, 0);
        gs_vk::vk.MapMemory(device, readbackMem, 0, req.size, 0, reinterpret_cast<void **>(&readbackMapped));
    }
    return true;
}

uint32_t GSVkBackend::Impl::memoryType(VkMemoryRequirements req, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties memProps{};
    gs_vk::vk.GetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

VkShaderModule GSVkBackend::Impl::shader(const char *name)
{
    const gs_vk_spv::Blob blob = gs_vk_spv::Get(name);
    if (!blob.data)
        return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = blob.size;
    ci.pCode = reinterpret_cast<const uint32_t *>(blob.data);
    VkShaderModule m = VK_NULL_HANDLE;
    gs_vk::vk.CreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

// ==== command buffers ==============================================================================

void GSVkBackend::Impl::beginFrameCb()
{
    if (cbOpen)
        return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    gs_vk::vk.BeginCommandBuffer(frameCb, &bi);
    cbOpen = true;
}

void GSVkBackend::Impl::endFrameCb()
{
    if (!cbOpen)
        return;
    gs_vk::vk.EndCommandBuffer(frameCb);
    cbOpen = false;
}

void GSVkBackend::Impl::submitAndWait()
{
    endFrameCb();
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.commandBufferCount = 1;
    su.pCommandBuffers = &frameCb;
    gs_vk::vk.ResetFences(device, 1, &frameFence);
    gs_vk::vk.QueueSubmit(queue, 1, &su, frameFence);
    gs_vk::vk.WaitForFences(device, 1, &frameFence, VK_TRUE, 1000000000ull);
    gs_vk::vk.ResetCommandBuffer(frameCb, 0);
    gs_vk::vk.ResetCommandPool(device, cmdPool, 0);
    emptyTrash();
}

void GSVkBackend::Impl::globalBarrier()
{
    VkMemoryBarrier bar{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    bar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    gs_vk::vk.CmdPipelineBarrier(frameCb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &bar, 0, nullptr, 0, nullptr);
}

void GSVkBackend::Impl::trash(void *obj, bool isMemory)
{
    trash_.emplace_back(obj, isMemory);
}

void GSVkBackend::Impl::emptyTrash()
{
    for (auto &[obj, isMemory] : trash_)
    {
        if (isMemory)
            gs_vk::vk.FreeMemory(device, reinterpret_cast<VkDeviceMemory>(obj), nullptr);
        else
            gs_vk::vk.DestroyBuffer(device, reinterpret_cast<VkBuffer>(obj), nullptr);
    }
    trash_.clear();
}

// ==== pipelines ====================================================================================

VkPipeline GSVkBackend::Impl::pipeline(const PipelineKey &key)
{
    const auto it = pipelines.find(key);
    if (it != pipelines.end())
        return it->second;
    VkPipeline p = buildPipeline(key);
    pipelines[key] = p;
    return p;
}

VkPipeline GSVkBackend::Impl::buildPipeline(const PipelineKey &key)
{
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    switch (key.program)
    {
    case Program::Draw: vs = vsDraw; fs = fsDraw; break;
    case Program::Shuffle: vs = vsShuffle; fs = fsShuffle; break;
    case Program::Present: vs = vsPresent; fs = fsPresent; break;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };

    VkVertexInputBindingDescription bind{0, sizeof(HwVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr[6] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HwVertex, x)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(HwVertex, z)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HwVertex, r)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HwVertex, s)},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HwVertex, u)},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HwVertex, cu0)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 6;
    vi.pVertexAttributeDescriptions = attr;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = key.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = key.depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = static_cast<VkCompareOp>(key.depthCompare);
    ds.minDepthBounds = ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = key.blendEnable;
    att.srcColorBlendFactor = static_cast<VkBlendFactor>(key.srcColor);
    att.dstColorBlendFactor = static_cast<VkBlendFactor>(key.dstColor);
    att.colorBlendOp = static_cast<VkBlendOp>(key.equation);
    att.srcAlphaBlendFactor = static_cast<VkBlendFactor>(key.srcAlpha);
    att.dstAlphaBlendFactor = static_cast<VkBlendFactor>(key.dstAlpha);
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    att.colorWriteMask = key.writeMask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dyn;
    gi.layout = drawPipelineLayout;
    gi.renderPass = key.program == Program::Present ? rpPresent : rpDraw;
    gi.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    if (gs_vk::vk.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gi, nullptr, &p) != VK_SUCCESS)
        std::fprintf(stderr, "[gs-vk] pipeline creation failed (program %d)\n", static_cast<int>(key.program));
    return p;
}

// ==== render targets ===============================================================================

GSVkBackend::Impl::RenderTarget *GSVkBackend::Impl::findTarget(uint32_t fbp)
{
    for (RenderTarget &t : targets)
        if (t.fbp == fbp)
            return &t;
    return nullptr;
}

GSVkBackend::Impl::RenderTarget &GSVkBackend::Impl::target(uint32_t fbp, uint32_t fbw)
{
    std::fprintf(stderr, "[gs-vk] target(%u,%u\n", fbp, fbw);
    for (RenderTarget &t : targets)
        if (t.fbp == fbp && t.fbw >= fbw)
            return t;
    for (RenderTarget &t : targets)
        if (t.fbp == fbp)
        {
            destroyTarget(t);
            createTarget(t, fbp, fbw);
            return t;
        }
    targets.emplace_back();
    createTarget(targets.back(), fbp, fbw);
    return targets.back();
}

void GSVkBackend::Impl::createTarget(RenderTarget &t, uint32_t fbp, uint32_t fbw)
{
    t.fbp = fbp;
    t.psm = 0;
    t.fbw = std::max<uint32_t>(fbw, 1u);
    t.pxW = t.fbw * 64u;
    t.pxH = kRtRows;
    t.zbp = ~0u;
    t.version = 1;
    t.lastDrawSeq = t.lastDepthSeq = 0;
    t.usedW = t.usedH = 0;
    t.depthCleared = false;
    t.cpu.clear();
    t.cpuVersion = 0;

    auto makeImage = [&](VkImage &img, VkDeviceMemory &mem, VkFormat fmt, VkImageUsageFlags usage) {
        VkImageCreateInfo imi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imi.imageType = VK_IMAGE_TYPE_2D;
        imi.format = fmt;
        imi.extent = {t.pxW, t.pxH, 1};
        imi.mipLevels = imi.arrayLayers = 1;
        imi.samples = VK_SAMPLE_COUNT_1_BIT;
        imi.tiling = VK_IMAGE_TILING_OPTIMAL;
        imi.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (gs_vk::vk.CreateImage(device, &imi, nullptr, &img) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        gs_vk::vk.GetImageMemoryRequirements(device, img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (gs_vk::vk.AllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS)
            return false;
        gs_vk::vk.BindImageMemory(device, img, mem, 0);
        return true;
    };
    if (!makeImage(t.color, t.colorMem, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ||
        !makeImage(t.depth, t.depthMem, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        fail("render target images");
        return;
    }
    auto makeView = [&](VkImageView &view, VkImage img, VkFormat fmt, VkImageAspectFlags aspect) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        return gs_vk::vk.CreateImageView(device, &vi, nullptr, &view) == VK_SUCCESS;
    };
    if (!makeView(t.colorAttach, t.color, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !makeView(t.colorSampled, t.color, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !makeView(t.colorInput, t.color, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !makeView(t.depthAttach, t.depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !makeView(t.depthSampled, t.depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT))
    {
        fail("render target views");
        return;
    }
    VkImageView views[2] = {t.colorAttach, t.depthAttach};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = rpDraw;
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = t.pxW;
    fi.height = t.pxH;
    fi.layers = 1;
    if (gs_vk::vk.CreateFramebuffer(device, &fi, nullptr, &t.fb) != VK_SUCCESS)
    {
        fail("render target framebuffer");
        return;
    }
    VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fc.renderPass = rpClearColorOnly;
    fc.attachmentCount = 1;
    fc.pAttachments = &views[0];
    fc.width = t.pxW;
    fc.height = t.pxH;
    fc.layers = 1;
    gs_vk::vk.CreateFramebuffer(device, &fc, nullptr, &t.fbColorOnly);

    // initial clear: colour 0, depth 0 (like the desktop renderer)
    beginFrameCb();
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = rpInitCD;
    rb.framebuffer = t.fb;
    rb.renderArea.extent = {t.pxW, t.pxH};
    VkClearValue vals[2]{};
    vals[0].color = {{0.f, 0.f, 0.f, 0.f}};
    vals[1].depthStencil = {0.0f, 0};
    rb.pClearValues = vals;
    rb.clearValueCount = 2;
        gs_vk::vk.CmdBeginRenderPass(frameCb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    gs_vk::vk.CmdEndRenderPass(frameCb);
    t.depthCleared = true;
    endFrameCb();
    submitAndWait();
    std::fprintf(stderr, "[gs-vk] render target fbp=%u fbw=%u -> %ux%u\n", fbp, t.fbw, t.pxW, t.pxH);
}

void GSVkBackend::Impl::destroyTarget(RenderTarget &t)
{
    if (t.fb) gs_vk::vk.DestroyFramebuffer(device, t.fb, nullptr);
    if (t.fbColorOnly) gs_vk::vk.DestroyFramebuffer(device, t.fbColorOnly, nullptr);
    if (t.colorAttach) gs_vk::vk.DestroyImageView(device, t.colorAttach, nullptr);
    if (t.colorSampled) gs_vk::vk.DestroyImageView(device, t.colorSampled, nullptr);
    if (t.colorInput) gs_vk::vk.DestroyImageView(device, t.colorInput, nullptr);
    if (t.depthAttach) gs_vk::vk.DestroyImageView(device, t.depthAttach, nullptr);
    if (t.depthSampled) gs_vk::vk.DestroyImageView(device, t.depthSampled, nullptr);
    if (t.color) gs_vk::vk.DestroyImage(device, t.color, nullptr);
    if (t.depth) gs_vk::vk.DestroyImage(device, t.depth, nullptr);
    if (t.colorMem) gs_vk::vk.FreeMemory(device, t.colorMem, nullptr);
    if (t.depthMem) gs_vk::vk.FreeMemory(device, t.depthMem, nullptr);
    t = {};
}

// ==== textures =====================================================================================

GSVkBackend::Impl::Texture &GSVkBackend::Impl::uploadTexture(uint64_t key, uint32_t w, uint32_t h, const void *rgba, uint32_t pitchBytes,
                                                             bool singleByte, uint32_t blockLo, uint32_t blockHi)
{
    Texture &tex = textures[key];
    tex.lastUse = ++useCounter;
    if (tex.valid)
        return tex;
    tex.width = w;
    tex.height = h;
    tex.blockLo = blockLo;
    tex.blockHi = blockHi;
    if (tex.image)
    {
        // stale image: recreate (size may have changed)
        trash(tex.image, false);
        trash(tex.mem, true);
        tex.image = VK_NULL_HANDLE;
        tex.mem = VK_NULL_HANDLE;
        tex.view = VK_NULL_HANDLE;
    }
    const VkDeviceSize bytes = singleByte ? static_cast<VkDeviceSize>(w) * h : static_cast<VkDeviceSize>(w) * h * 4;
    auto a = staging.alloc(bytes, 16);
    if (!a.cpu)
    {
        submitAndWait();
        staging.reset();
        a = staging.alloc(bytes, 16);
        if (!a.cpu)
            return tex;
    }
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t *>(a.cpu) + static_cast<size_t>(y) * w * (singleByte ? 1u : 4u),
                    static_cast<const uint8_t *>(rgba) + static_cast<size_t>(y) * pitchBytes,
                    static_cast<size_t>(w) * (singleByte ? 1u : 4u));

    VkImageCreateInfo imi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imi.imageType = VK_IMAGE_TYPE_2D;
    imi.format = singleByte ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    imi.extent = {w, h, 1};
    imi.mipLevels = imi.arrayLayers = 1;
    imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (gs_vk::vk.CreateImage(device, &imi, nullptr, &tex.image) != VK_SUCCESS)
        return tex;
    VkMemoryRequirements req{};
    gs_vk::vk.GetImageMemoryRequirements(device, tex.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (gs_vk::vk.AllocateMemory(device, &ai, nullptr, &tex.mem) != VK_SUCCESS)
        return tex;
    gs_vk::vk.BindImageMemory(device, tex.image, tex.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = tex.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = imi.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    gs_vk::vk.CreateImageView(device, &vi, nullptr, &tex.view);

    beginFrameCb();
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.image = tex.image;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    gs_vk::vk.CmdPipelineBarrier(frameCb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {w, h, 1};
    gs_vk::vk.CmdCopyBufferToImage(frameCb, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    VkImageMemoryBarrier bar2 = bar;
    bar2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    gs_vk::vk.CmdPipelineBarrier(frameCb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar2);
    tex.valid = true;
    if (textures.size() > 6000u)
        evictOld();
    return tex;
}

// Texels of a direct-colour texture read from GS memory with the swizzle tables, alpha expanded by TEXA like the GS
// texture unit (CT24: TA0 / AEM, CT16: TA0/TA1 by the alpha bit).
void GSVkBackend::Impl::decodeDirect(const GSDrawState &s, std::vector<uint32_t> &out)
{
    const GSTex0Reg &t = s.context.tex0;
    const GSSwizzle::Format &f = GSSwizzle::GetFormat(t.psm);
    const uint32_t w = std::max<uint32_t>(1u, s.textureWidth), h = std::max<uint32_t>(1u, s.textureHeight);
    const bool sixteen = f.kind == GSSwizzle::Kind::Bits16;
    if (!vram || !f.table || !(sixteen || f.kind == GSSwizzle::Kind::Bits32 || f.kind == GSSwizzle::Kind::Bits24))
    {
        store.DecodeTextureRgba(s, out);
        return;
    }
    out.resize(static_cast<size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint32_t *line = out.data() + static_cast<size_t>(y) * w;
        if (!GSMem::ReadColorRun(vram, t.psm, t.tbp0, t.tbw, 0u, y, w, line))
        {
            const GSSwizzle::AxisPart row = GSSwizzle::RowPart(f, t.tbp0, t.tbw, y);
            for (uint32_t x = 0; x < w; ++x)
            {
                const GSSwizzle::Location at = GSSwizzle::Combine(f, row, GSSwizzle::ColumnPart(f, x));
                line[x] = GSSwizzle::LoadAt(f, vram + at.byte, at.shift);
            }
        }
        for (uint32_t x = 0; x < w; ++x)
            line[x] = GSCpuInternal::applyTexa(s.texa, t.psm, sixteen ? GSCpuInternal::Rgba5551ToRgba8888(static_cast<uint16_t>(line[x])) : line[x]);
    }
}

GSVkBackend::Impl::Texture &GSVkBackend::Impl::textureFor(const GSDrawState &s)
{
    const GSTex0Reg &t = s.context.tex0;
    uint64_t key = t.tbp0 | (static_cast<uint64_t>(t.tbw) << 14) | (static_cast<uint64_t>(t.psm & 63) << 20) |
                   (static_cast<uint64_t>(s.textureWidth) << 26) | (static_cast<uint64_t>(s.textureHeight) << 38);
    key = key * 1099511628211ull ^ (static_cast<uint64_t>(s.texa.ta0) | (static_cast<uint64_t>(s.texa.ta1) << 8) | (static_cast<uint64_t>(s.texa.aem) << 16));
    if (isIndexedPsm(t.psm))
    {
        key = key * 1099511628211ull ^ clutHash;
        key = key * 1099511628211ull ^ (t.cpsm | (t.csm << 8) | (t.csa << 16));
    }
    Texture &tex = textures[key];
    tex.lastUse = ++useCounter;
    if (tex.valid)
        return tex;
    std::vector<uint32_t> pixels;
    decodeDirect(s, pixels);
    const uint32_t w = std::max<uint32_t>(1u, s.textureWidth), h = std::max<uint32_t>(1u, s.textureHeight);
    return uploadTexture(key, w, h, pixels.data(), w * 4, false, t.tbp0, t.tbp0 + spanBlocks(t.psm, t.tbw, h));
}

// Index texture of a paletted format: the raw indices read from GS memory. Independent of the CLUT, so palette changes
// (animated CLUTs, one image with several palettes) cost nothing.
GSVkBackend::Impl::Texture &GSVkBackend::Impl::indexTextureFor(const GSDrawState &s)
{
    const GSTex0Reg &t = s.context.tex0;
    const uint64_t key = (1ull << 63) | t.tbp0 | (static_cast<uint64_t>(t.tbw) << 14) | (static_cast<uint64_t>(t.psm & 63) << 20) |
                         (static_cast<uint64_t>(s.textureWidth) << 26) | (static_cast<uint64_t>(s.textureHeight) << 38);
    Texture &tex = textures[key];
    tex.lastUse = ++useCounter;
    if (tex.valid)
        return tex;
    const uint32_t w = std::max<uint32_t>(1u, s.textureWidth), h = std::max<uint32_t>(1u, s.textureHeight);
    std::vector<uint8_t> indices(static_cast<size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        GSMem::ReadIndexRun(vram, t.psm, t.tbp0, t.tbw, 0u, y, w, indices.data() + static_cast<size_t>(y) * w);
    tex.width = w;
    tex.height = h;
    tex.blockLo = t.tbp0;
    tex.blockHi = t.tbp0 + spanBlocks(t.psm, t.tbw, h);
    return uploadTexture(key, w, h, indices.data(), w, true, tex.blockLo, tex.blockHi);
}

// 256x1 RGBA palette of the current CLUT as the draw reads it (format, offset, TEXA alpha)
GSVkBackend::Impl::Texture &GSVkBackend::Impl::paletteFor(const GSDrawState &s)
{
    const GSTex0Reg &t = s.context.tex0;
    const auto paletteKey = [&]
    {
        uint64_t key = clutHash;
        key = key * 1099511628211ull ^ (t.cpsm | (t.csm << 8) | (t.csa << 16) | (static_cast<uint64_t>(t.psm & 63) << 24));
        key = key * 1099511628211ull ^ (static_cast<uint64_t>(s.texa.ta0) | (static_cast<uint64_t>(s.texa.ta1) << 8) | (static_cast<uint64_t>(s.texa.aem) << 16));
        return key;
    };
    if (palettes.size() > 4096u)
    {
        for (auto &kv : palettes)
            if (kv.second.image)
            {
                trash(kv.second.image, false);
                trash(kv.second.mem, true);
            }
        palettes.clear();
    }
    const uint64_t key = paletteKey();
    Texture &tex = palettes[key];
    tex.lastUse = ++useCounter;
    if (tex.valid)
        return tex;
    uint32_t colors[256];
    store.BuildPalette(s, colors);
    return uploadTexture(key, 256, 1, colors, 256 * 4, false, 0, 0);
}

void GSVkBackend::Impl::evictOld()
{
    for (auto it = textures.begin(); it != textures.end();)
    {
        if (it->second.valid && it->second.lastUse + 3000u < useCounter)
        {
            if (it->second.image)
                trash(it->second.image, false);
            if (it->second.mem)
                trash(it->second.mem, true);
            it = textures.erase(it);
        }
        else
            ++it;
    }
}

void GSVkBackend::Impl::invalidateBlocks(uint32_t lo, uint32_t hi)
{
    for (auto &kv : textures)
    {
        Texture &t = kv.second;
        if (t.valid && t.blockLo < hi && lo < t.blockHi)
            t.valid = false;
    }
}

// ==== CLUT from a render target ====================================================================

// The render target whose memory range holds the texture's first block (and was not overwritten by an upload since).

bool GSVkBackend::Impl::readTargetCpu(RenderTarget &rt)
{
    if (rt.cpuVersion == rt.version && !rt.cpu.empty())
        return true;
    if (rt.cpu.size() != static_cast<size_t>(rt.pxW) * rt.pxH * 4u)
        rt.cpu.resize(static_cast<size_t>(rt.pxW) * rt.pxH * 4u);
    // pending draws may still be open: submit them so the image holds the picture
    flushPending();
    submitAndWait();
    beginFrameCb();
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {rt.pxW, rt.pxH, 1};
gs_vk::vk.CmdCopyImageToBuffer(frameCb, rt.color, VK_IMAGE_LAYOUT_GENERAL, readbackBuffer, 1, &cp);
endFrameCb();
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.commandBufferCount = 1;
    su.pCommandBuffers = &frameCb;
    gs_vk::vk.ResetFences(device, 1, &frameFence);
    gs_vk::vk.QueueSubmit(queue, 1, &su, frameFence);
    gs_vk::vk.WaitForFences(device, 1, &frameFence, VK_TRUE, 1000000000ull);
    gs_vk::vk.ResetCommandBuffer(frameCb, 0);
    gs_vk::vk.ResetCommandPool(device, cmdPool, 0);
    cbOpen = false;
    emptyTrash();
    std::memcpy(rt.cpu.data(), readbackMapped, rt.cpu.size());
    rt.cpuVersion = rt.version;
    return true;
}

// A CLUT whose source memory a render target was drawn into is rebuilt on the
// CPU: read the target back, then apply the same reinterpreting address math
// as the GPU fetch (the desktop renderer does this in a GPU pass; the result
// is the raw GS memory word per entry either way).
bool GSVkBackend::Impl::clutFromTarget(const GSTex0Reg &tex0, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words)
{
    RenderTarget *rt = findTarget(tex0.cbp);
    for (RenderTarget &t : targets)
    {
        // prefer a target that owns this memory and has drawn content
        if (t.fbp == tex0.cbp && t.usedW != 0u)
        {
            rt = &t;
            break;
        }
    }
    if (!rt || count == 0u || count > 256u)
        return false;
    if (!readTargetCpu(*rt))
        return false;
    const GSSwizzle::Format &tf = GSSwizzle::GetFormat(tex0.cpsm);
    const GSSwizzle::Format &rf = GSSwizzle::GetFormat(rt->psm);
    if (!tf.table || !rf.table || rf.kind == GSSwizzle::Kind::Invalid)
        return false;
    const auto inv = inverseOffset.find(rt->psm);
    if (inv == inverseOffset.end())
        return false;
    const uint32_t texelBits = tf.kind == GSSwizzle::Kind::Bits32 ? 32 : tf.kind == GSSwizzle::Kind::Bits24 ? 24 : 16;
    const uint32_t B = 1u << rf.unpackedShift; // bits per RT pixel
    const uint32_t pageShift = rf.pixelsPerPageShift;
    const uint32_t rtBase = rt->fbp << pageShift;
    const uint32_t pageW = 1u << rf.pageShiftX, pageH = 1u << rf.pageShiftY;
    const std::vector<uint16_t> &invTable = inverseFor(rt->psm);
    const uint32_t rtRgbaStride = rt->pxW;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t x = xy[i * 2u], y = xy[i * 2u + 1u];
        const uint32_t pixelIndex = GSSwizzle::PixelIndex(tf, tex0.cbp, width, x, y);
        const uint32_t bitpos = (pixelIndex << tf.unpackedShift) + tf.bitOffset;
        const uint32_t rtIdx = bitpos / B;
        const uint32_t within = bitpos % B;
        if (rtIdx < rtBase)
        {
            words[i] = 0u;
            continue;
        }
        const uint32_t rel = rtIdx - rtBase;
        const uint32_t rpage = rel >> pageShift;
        const uint32_t inpage = rel & ((1u << pageShift) - 1u);
        const uint32_t e = invTable[inpage];
        const uint32_t px = (rpage % rt->fbw) * pageW + (e & (pageW - 1u));
        const uint32_t py = (rpage / rt->fbw) * pageH + (e >> rf.pageShiftX);
        uint32_t word = 0u;
        if (px < rt->pxW && py < rt->pxH)
        {
            const uint8_t *p = &rt->cpu[(static_cast<size_t>(py) * rtRgbaStride + px) * 4u];
            if (B == 32u)
                word = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
            else
                word = ((p[3] & 0x80u) ? 0x8000u : 0u) | ((p[2] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[0] >> 3);
        }
        words[i] = (texelBits >= 32u) ? (word >> within) : ((word >> within) & ((1u << texelBits) - 1u));
    }
    return true;
}

// ==== drawing ======================================================================================

void GSVkBackend::Impl::addVertex(const GSDrawState &s, const GSVertex &v, float u, float tv, float zmax)
{
    const GSContext &c = s.context;
    if (pending.empty())
        lastSprite.end = 0; // (firstRawZ bookkeeping not needed at 1x)
    const float half = 0.5f / kScale;
    HwVertex h;
    h.x = v.x - static_cast<float>(c.xyoffset.ofx) / 16.0f + half;
    h.y = v.y - static_cast<float>(c.xyoffset.ofy) / 16.0f + half;
    double zq = std::max(v.z, 0.0);
    if (zmax < 4294967295.0f)
        zq = static_cast<double>(static_cast<uint64_t>(zq) & (zmax < 70000.0f ? 0xFFFFull : 0xFFFFFFull));
    h.z = static_cast<float>(std::min(zq / zmax, 1.0));
    h.r = v.r; h.g = v.g; h.b = v.b; h.a = v.a;
    h.s = v.s; h.t = v.t; h.q = v.q;
    h.u = u; h.v = tv;
    pending.push_back(h);
}

void GSVkBackend::Impl::submit(const GSPrimitiveBatch &b)
{
    const GSDrawState &s = b.state;
    const GSPrimType type = s.prim.type;
    const bool tri = type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN;
    const bool sprite = type == GS_PRIM_SPRITE;
    const bool line = type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP;
    if (!tri && !sprite && !line)
        return;
    const uint8_t fpsm = s.context.frame.psm;
    if (family16(fpsm) && tryShuffle(s, b))
        return;
    flushShuffle();
    if (((fpsm & 0x3F) == 0x30 || (fpsm & 0x3F) == 0x31 || (fpsm & 0x3F) == 0x32 || (fpsm & 0x3F) == 0x3A) && sprite && depthClearSprite(s, b))
        return;
    if (fpsm != GS_PSM_CT32 && fpsm != GS_PSM_CT24 && fpsm != GS_PSM_CT16 && fpsm != GS_PSM_CT16S)
        return;
    for (const RenderTarget &t : targets)
        if (t.fbp == s.context.frame.fbp && t.usedW != 0u && family16(t.psm) != family16(fpsm))
            return;
    if (havePending && !sameState(pendingState, s))
        flushPending();
    if (!havePending)
    {
        pendingState = s;
        havePending = true;
    }
    const float zmax = zMaxFor(s.context.zbuf.psm);
    const float texW = static_cast<float>(std::max<uint16_t>(s.textureWidth, 1)), texH = static_cast<float>(std::max<uint16_t>(s.textureHeight, 1));
    const auto texelUV = [&](const GSVertex &v, float &u, float &t)
    {
        if (s.prim.fst)
        {
            u = static_cast<float>(v.u) / 16.0f;
            t = static_cast<float>(v.v) / 16.0f;
        }
        else
        {
            u = 0.0f;
            t = 0.0f;
        }
    };
    if (line)
    {
        const GSVertex &a = b.vertices[0], &c = b.vertices[1];
        float dx = c.x - a.x, dy = c.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f)
            dx = 1.0f, dy = 0.0f;
        else
            dx /= len, dy /= len;
        const float ext = len < 1.0f ? (1.0f - len) * 0.5f : 0.0f;
        const float nx = -dy * 0.5f, ny = dx * 0.5f;
        GSVertex e[2] = {a, c};
        e[0].x -= dx * ext; e[0].y -= dy * ext;
        e[1].x += dx * ext; e[1].y += dy * ext;
        const int order[6][2] = {{0, 1}, {1, 1}, {0, -1}, {1, 1}, {1, -1}, {0, -1}};
        const float centre = 0.5f - 0.5f / kScale;
        for (const auto &o : order)
        {
            GSVertex v = e[o[0]];
            v.x += nx * static_cast<float>(o[1]) + centre * std::fabs(dy);
            v.y += ny * static_cast<float>(o[1]) + centre * std::fabs(dx);
            float u, t;
            texelUV(v, u, t);
            addVertex(s, v, u, t, zmax);
        }
    }
    else if (tri)
    {
        for (int i = 0; i < 3; ++i)
        {
            float u, t;
            texelUV(b.vertices[i], u, t);
            addVertex(s, b.vertices[i], u, t, zmax);
        }
    }
    else
    {
        const GSVertex &a = b.vertices[0];
        const GSVertex &c = b.vertices[1];
        float u0, v0, u1, v1;
        if (s.prim.fst)
        {
            u0 = a.u / 16.0f; v0 = a.v / 16.0f; u1 = c.u / 16.0f; v1 = c.v / 16.0f;
        }
        else
        {
            const float q0 = std::fabs(a.q) < 1e-20f ? 1e-20f : a.q, q1 = std::fabs(c.q) < 1e-20f ? 1e-20f : c.q;
            u0 = a.s / q0 * texW; v0 = a.t / q0 * texH; u1 = c.s / q1 * texW; v1 = c.t / q1 * texH;
        }
        float xs[2] = {a.x, c.x}, ys[2] = {a.y, c.y};
        float us[2] = {u0, u1}, vs[2] = {v0, v1};
        const auto snap = [](float (&p)[2], float (&t)[2], float off)
        {
            const float w0 = p[0] - off, w1 = p[1] - off;
            const float s0 = std::ceil(w0), s1 = std::ceil(w1);
            if (w0 == w1 || s0 == s1)
                return false;
            const float k = (t[1] - t[0]) / (w1 - w0);
            t[0] += (s0 - w0) * k;
            t[1] += (s1 - w1) * k;
            p[0] = s0 + off;
            p[1] = s1 + off;
            return true;
        };
        const bool coversX = snap(xs, us, s.context.xyoffset.ofx / 16.0f);
        const bool coversY = snap(ys, vs, s.context.xyoffset.ofy / 16.0f);
        if (!coversX || !coversY)
            return;
        const auto sampled = [](float p0, float p1, float t0, float t1, float &lo, float &hi)
        {
            lo = std::min(t0, t1);
            hi = std::max(t0, t1);
            const float first = std::ceil(std::min(p0, p1)), last = std::ceil(std::max(p0, p1)) - 1.0f;
            if (last < first || p1 == p0)
                return;
            const float ta = t0 + (first - p0) * (t1 - t0) / (p1 - p0), tb = t0 + (last - p0) * (t1 - t0) / (p1 - p0);
            lo = std::min(ta, tb);
            hi = std::max(ta, tb);
        };
        float cu0, cu1, cv0, cv1;
        sampled(a.x - s.context.xyoffset.ofx / 16.0f, c.x - s.context.xyoffset.ofx / 16.0f, u0, u1, cu0, cu1);
        sampled(a.y - s.context.xyoffset.ofy / 16.0f, c.y - s.context.xyoffset.ofy / 16.0f, v0, v1, cv0, cv1);
        struct SpriteEdges
        {
            float xs[2], ys[2], us[2], vs[2];
        };
        SpriteEdges cur{{xs[0], xs[1]}, {ys[0], ys[1]}, {us[0], us[1]}, {vs[0], vs[1]}};
        if (lastSprite.batch == batchSerial && lastSprite.end == pending.size() && lastSprite.end >= 6)
        {
            // open the clamp on the side two consecutive sprites share when the texture continues across it
            const auto same = [](const float a[2], const float b[2], float tol)
            {
                return std::fabs(std::min(a[0], a[1]) - std::min(b[0], b[1])) <= tol &&
                       std::fabs(std::max(a[0], a[1]) - std::max(b[0], b[1])) <= tol;
            };
            constexpr float kXyTol = 1.0f / 64.0f, kUvTol = 1.0f / 32.0f;
            const auto seam = [&](const float p[2], const float pt[2], const float c2[2], const float ct[2], int &ip, int &ic)
            {
                for (ip = 0; ip < 2; ++ip)
                    for (ic = 0; ic < 2; ++ic)
                        if (std::fabs(p[ip] - c2[ic]) <= kXyTol && std::fabs(pt[ip] - ct[ic]) <= kUvTol &&
                            (p[1 - ip] - p[ip]) * (c2[1 - ic] - c2[ic]) < 0.0f)
                            return true;
                return false;
            };
            int ip = 0, ic = 0;
            float *prevV = &pending[lastSprite.end - 6].cu0;
            const SpriteEdges &prev = *reinterpret_cast<const SpriteEdges *>(&lastSprite);
            if (same(prev.ys, cur.ys, kXyTol) && same(prev.vs, cur.vs, kUvTol) && seam(prev.xs, prev.us, cur.xs, cur.us, ip, ic))
            {
                const bool prevHigh = prev.us[ip] > prev.us[1 - ip], curHigh = cur.us[ic] > cur.us[1 - ic];
                for (int i = 0; i < 6; ++i)
                    (prevHigh ? prevV[i * 4 + 2] : prevV[i * 4]) = prevHigh ? 1e30f : -1e30f;
                (curHigh ? cu1 : cu0) = curHigh ? 1e30f : -1e30f;
            }
            else if (same(prev.xs, cur.xs, kXyTol) && same(prev.us, cur.us, kUvTol) && seam(prev.ys, prev.vs, cur.ys, cur.vs, ip, ic))
            {
                const bool prevHigh = prev.vs[ip] > prev.vs[1 - ip], curHigh = cur.vs[ic] > cur.vs[1 - ic];
                for (int i = 0; i < 6; ++i)
                    (prevHigh ? prevV[i * 4 + 3] : prevV[i * 4 + 1]) = prevHigh ? 1e30f : -1e30f;
                (curHigh ? cv1 : cv0) = curHigh ? 1e30f : -1e30f;
            }
        }
        constexpr float kShrinkUnclamped = 4.0f;
        if (a.x != c.x && std::fabs((u1 - u0) / (c.x - a.x)) > kShrinkUnclamped)
            cu0 = -1e30f, cu1 = 1e30f;
        if (a.y != c.y && std::fabs((v1 - v0) / (c.y - a.y)) > kShrinkUnclamped)
            cv0 = -1e30f, cv1 = 1e30f;
        const int order[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
        for (const auto &o : order)
        {
            GSVertex v = c; // colour, z, fog come from the second vertex
            v.x = xs[o[0]];
            v.y = ys[o[1]];
            addVertex(s, v, us[o[0]], vs[o[1]], zmax);
            pending.back().s = us[o[0]] / texW;
            pending.back().t = vs[o[1]] / texH;
            pending.back().q = 1.0f;
            pending.back().cu0 = cu0;
            pending.back().cv0 = cv0;
            pending.back().cu1 = cu1;
            pending.back().cv1 = cv1;
        }
        lastSprite.xs[0] = xs[0]; lastSprite.xs[1] = xs[1];
        lastSprite.ys[0] = ys[0]; lastSprite.ys[1] = ys[1];
        lastSprite.us[0] = us[0]; lastSprite.us[1] = us[1];
        lastSprite.vs[0] = vs[0]; lastSprite.vs[1] = vs[1];
        lastSprite.end = pending.size();
        lastSprite.batch = batchSerial;
    }
}

void GSVkBackend::Impl::blendState(const GSDrawState &s, bool &enabled, uint32_t &sf, uint32_t &df, float &constant, uint32_t &equation, bool &soft)
{
    // GL factor constants mirrored by Vulkan (same numeric values).
    constexpr uint32_t kZero = VK_BLEND_FACTOR_ZERO, kOne = VK_BLEND_FACTOR_ONE;
    constexpr uint32_t kSrc1Alpha = VK_BLEND_FACTOR_SRC1_ALPHA, kOneMinusSrc1Alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    constexpr uint32_t kConstantColor = VK_BLEND_FACTOR_CONSTANT_COLOR, kOneMinusConstantColor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    constexpr uint32_t kFuncAdd = VK_BLEND_OP_ADD, kFuncReverseSubtract = VK_BLEND_OP_REVERSE_SUBTRACT;
    enabled = false;
    soft = false;
    equation = kFuncAdd;
    sf = kOne; df = kZero;
    constant = 0.0f;
    if (!s.prim.abe)
        return;
    const uint64_t alpha = s.context.alpha;
    const uint32_t a = alpha & 3, b = (alpha >> 2) & 3, c = (alpha >> 4) & 3, d = (alpha >> 6) & 3;
    const uint32_t fix = static_cast<uint32_t>((alpha >> 32) & 0xFF);
    uint32_t cs, cd;
    if (c == 0) { cs = kSrc1Alpha; cd = kOneMinusSrc1Alpha; }
    else if (c == 2) { cs = kConstantColor; cd = kOneMinusConstantColor; constant = std::min(fix / 128.0f, 1.0f); }
    else { soft = true; cs = kSrc1Alpha; cd = kOneMinusSrc1Alpha; }
    if ((s.colclamp & 1u) == 0u && a != b)
        soft = true;
    if (a == b)
    {
        if (d == 0) return;
        enabled = true; sf = kZero; df = kOne; return;
    }
    enabled = true;
    if (a == 0 && b == 1 && d == 1) { sf = cs; df = cd; }
    else if (a == 0 && b == 2 && d == 1) { sf = cs; df = kOne; }
    else if (a == 2 && b == 1 && d == 1) { sf = kZero; df = cd; }
    else if (a == 0 && b == 2 && d == 2) { sf = cs; df = kZero; }
    else if (a == 2 && b == 0 && d == 2) { sf = kZero; df = kZero; }
    else if (a == 2 && b == 0 && d == 1) { sf = cs; df = kOne; equation = kFuncReverseSubtract; }
    else { soft = true; sf = cs; df = cd; }
    if (!dualSrc && (cs == kSrc1Alpha || cd == kOneMinusSrc1Alpha))
        soft = true;
}

bool GSVkBackend::Impl::isZInversion(const GSDrawState &s, const std::vector<HwVertex> &verts)
{
    const GSContext &c = s.context;
    const uint64_t al = c.alpha;
    if (c.frame.fbp != c.zbuf.zbp || c.frame.psm != GS_PSM_CT24 || s.prim.tme || !s.prim.abe || c.frame.fbmsk != 0u ||
        (al & 0xFFu) != 0xA4u || ((al >> 32) & 0xFFu) != 0x80u || verts.empty())
        return false;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const HwVertex &v : verts)
    {
        if (v.r < 254.5f || v.g < 254.5f || v.b < 254.5f)
            return false;
        x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y);
    }
    return x0 <= c.scissor.x0 + 1.0f && y0 <= c.scissor.y0 + 1.0f && x1 >= c.scissor.x1 && y1 >= c.scissor.y1;
}

bool GSVkBackend::Impl::depthRowOf(const RenderTarget &t, uint32_t page, uint32_t fbw, uint8_t zpsm, int &rowOffset)
{
    if (t.zbp == ~0u || page < t.zbp || std::max<uint32_t>(fbw, 1u) != t.fbw)
        return false;
    const uint32_t delta = page - t.zbp;
    if (delta % t.fbw != 0u)
        return false;
    rowOffset = static_cast<int>(delta / t.fbw) * ((zpsm == 0x32 || zpsm == 0x3A) ? 64 : 32);
    return rowOffset < static_cast<int>(t.pxH);
}

// An untextured sprite drawn with a Z format into the memory of a target's Z buffer writes Z words directly (God of War
// II clears its Z buffer this way between the sky and the world).
bool GSVkBackend::Impl::depthClearSprite(const GSDrawState &s, const GSPrimitiveBatch &b)
{
    const GSContext &c = s.context;
    if (s.prim.tme || c.frame.fbmsk != 0u)
        return false;
    const GSVertex &v = b.vertices[1];
    const bool zWrites = ((c.test >> 16) & 1u) != 0u && ((c.test >> 17) & 3u) == 1u && !c.zbuf.zmask;
    bool any = false;
    for (const RenderTarget &t : targets)
    {
        int row = 0;
        any = any || depthRowOf(t, c.frame.fbp, c.frame.fbw, c.frame.psm, row) ||
              (zWrites && depthRowOf(t, c.zbuf.zbp, c.frame.fbw, c.zbuf.psm, row));
    }
    if (!any)
        return false;
    flushPending();
    if (!ensure())
        return true;
    double frameZ;
    if (c.frame.psm == 0x32 || c.frame.psm == 0x3A)
        frameZ = static_cast<double>((v.r >> 3) | ((v.g >> 3) << 5) | ((v.b >> 3) << 10) | ((v.a >> 7) << 15));
    else
        frameZ = static_cast<double>(static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16) |
                                     (c.frame.psm == 0x30 ? (static_cast<uint32_t>(v.a) << 24) : 0u));
    const float ox = static_cast<float>(c.xyoffset.ofx) / 16.0f, oy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
    const int x0 = std::max<int>(c.scissor.x0, static_cast<int>(std::ceil(std::min(b.vertices[0].x, v.x) - ox)));
    const int y0 = std::max<int>(c.scissor.y0, static_cast<int>(std::ceil(std::min(b.vertices[0].y, v.y) - oy)));
    const int x1 = std::min<int>(c.scissor.x1 + 1, static_cast<int>(std::ceil(std::max(b.vertices[0].x, v.x) - ox)));
    const int y1 = std::min<int>(c.scissor.y1 + 1, static_cast<int>(std::ceil(std::max(b.vertices[0].y, v.y) - oy)));
    if (x1 <= x0 || y1 <= y0)
        return true;
    beginFrameCb();
    const auto clearRows = [&](RenderTarget &t, int rowOffset, double z, uint8_t zpsm)
    {
        const int ty0 = y0 + rowOffset, ty1 = std::min<int>(y1 + rowOffset, static_cast<int>(t.pxH));
        if (ty1 <= ty0)
            return;
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil.depth = static_cast<float>(std::min(std::max(z, 0.0) / static_cast<double>(zMaxFor(zpsm)), 1.0));
        VkClearRect rect{};
        rect.rect.offset = {x0, ty0};
        rect.rect.extent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(ty1 - ty0)};
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = rpDraw;
        rb.framebuffer = t.fb;
        rb.renderArea.extent = {t.pxW, t.pxH};
        gs_vk::vk.CmdBeginRenderPass(frameCb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        gs_vk::vk.CmdClearAttachments(frameCb, 1, &clear, 1, &rect);
        
        gs_vk::vk.CmdEndRenderPass(frameCb);
        t.lastDepthSeq = ++writeSeq;
        const auto inv = std::find(invertedZ.begin(), invertedZ.end(), t.zbp);
        if (inv != invertedZ.end())
            invertedZ.erase(inv);
    };
    for (RenderTarget &t : targets)
    {
        int row = 0;
        if (depthRowOf(t, c.frame.fbp, c.frame.fbw, c.frame.psm, row))
            clearRows(t, row, frameZ, c.frame.psm);
        if (zWrites && depthRowOf(t, c.zbuf.zbp, c.frame.fbw, c.zbuf.psm, row))
            clearRows(t, row, std::min(v.z, static_cast<double>(zMaxFor(c.zbuf.psm))), c.zbuf.psm);
    }
    return true;
}

void GSVkBackend::Impl::flushPending()
{
    ++batchSerial;
    flushShuffle();
    if (!havePending || pending.empty())
    {
        havePending = false;
        pending.clear();
        return;
    }
    havePending = false;
    if (!ensure())
    {
        pending.clear();
        return;
    }
    draw(pendingState, pending);
    pending.clear();
}

void GSVkBackend::Impl::draw(const GSDrawState &s, const std::vector<HwVertex> &verts)
{
    const GSContext &c = s.context;
    RenderTarget &rt = target(c.frame.fbp, c.frame.fbw);
    if (failed)
        return;
    rt.psm = c.frame.psm;
    ++rt.version;
    rt.lastDrawSeq = ++writeSeq;
    {
        float maxX = 0.0f, maxY = 0.0f;
        for (const HwVertex &v : verts)
        {
            maxX = std::max(maxX, v.x);
            maxY = std::max(maxY, v.y);
        }
        const uint32_t bx = static_cast<uint32_t>(std::min<float>(std::max(maxX, 0.0f), static_cast<float>(c.scissor.x1 + 1u)));
        const uint32_t by = static_cast<uint32_t>(std::min<float>(std::max(maxY, 0.0f), static_cast<float>(c.scissor.y1 + 1u)));
        rt.usedW = std::max<uint32_t>(rt.usedW, std::min<uint32_t>(bx, rt.pxW));
        rt.usedH = std::max<uint32_t>(rt.usedH, std::min<uint32_t>(by, rt.pxH));
    }

    beginFrameCb();

    // depth
    const uint32_t ztst = ((c.test >> 16) & 1u) ? static_cast<uint32_t>((c.test >> 17) & 3u) : 1u;
    const bool zAlias = c.zbuf.zbp == c.frame.fbp && ((c.test >> 16) & 1u) != 0u &&
                        (c.frame.psm == GS_PSM_CT32 || c.frame.psm == GS_PSM_CT24);
    const bool zInv = !zAlias && ((c.test >> 16) & 1u) != 0u && zInverted(c.zbuf.zbp);
    std::vector<HwVertex> flippedZ;
    if (zInv)
    {
        flippedZ = verts;
        for (HwVertex &v : flippedZ)
            v.z = 1.0f - v.z;
    }
    const std::vector<HwVertex> &dverts = zInv ? flippedZ : verts;
    if (isZInversion(s, verts))
    {
        if (zInverted(c.zbuf.zbp))
            invertedZ.erase(std::find(invertedZ.begin(), invertedZ.end(), c.zbuf.zbp));
        else
            invertedZ.push_back(c.zbuf.zbp);
    }
    const bool zwrite = !c.zbuf.zmask && !zAlias;
    if (zwrite && ((c.test >> 16) & 1u) != 0u)
        rt.lastDepthSeq = rt.lastDrawSeq;
    const bool zAliasWrite = zAlias && !c.zbuf.zmask;

    // colour mask
    const uint32_t fbmsk = c.frame.fbmsk;
    const uint32_t writeMask =
        (((fbmsk & 0xFF) == 0xFF ? 0u : VK_COLOR_COMPONENT_R_BIT) |
         ((fbmsk & 0xFF00) == 0xFF00 ? 0u : VK_COLOR_COMPONENT_G_BIT) |
         ((fbmsk & 0xFF0000) == 0xFF0000 ? 0u : VK_COLOR_COMPONENT_B_BIT) |
         ((fbmsk & 0xFF000000u) == 0xFF000000u ? 0u : VK_COLOR_COMPONENT_A_BIT));

    bool blend = false, softBlend = false;
    uint32_t sf, df, equation;
    float constant;
    blendState(s, blend, sf, df, constant, equation, softBlend);
    if (softBlend || zAliasWrite)
        blend = false;

    // descriptor set
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &drawSetLayout;
    VkDescriptorSet dsc = VK_NULL_HANDLE;
    if (gs_vk::vk.AllocateDescriptorSets(device, &dai, &dsc) != VK_SUCCESS)
    {
        submitAndWait();
        dai.descriptorPool = descPool;
        gs_vk::vk.AllocateDescriptorSets(device, &dai, &dsc);
    }

    // UBO (local value; copied to the ring once its final offset is known)
    DrawUbo u{};
    u.uInvSize[0] = 1.0f / rt.pxW; u.uInvSize[1] = 1.0f / rt.pxH;
    u.uPxScale = kScale;
    u.uZAliasMax = zMaxFor(c.zbuf.psm);
    const bool date = ((c.test >> 14) & 1u) != 0u && c.frame.psm != GS_PSM_CT24;
    u.uDate = date ? 1 : 0;
    u.uDatm = ((c.test >> 15) & 1u) != 0u ? 1 : 0;
    u.uSwBlend = (softBlend && !zAliasWrite) ? 1 : 0;
    u.uColClamp = (s.colclamp & 1u) != 0u ? 1 : 0;
    const uint64_t al = c.alpha;
    u.uAbcd[0] = static_cast<int32_t>(al & 3u);
    u.uAbcd[1] = static_cast<int32_t>((al >> 2) & 3u);
    u.uAbcd[2] = static_cast<int32_t>((al >> 4) & 3u);
    u.uAbcd[3] = static_cast<int32_t>((al >> 6) & 3u);
    u.uFix = static_cast<float>((al >> 32) & 0xFFu);
    u.uZAlias = zAlias ? 1 + static_cast<int32_t>(ztst) : 0;
    u.uZAliasWrite = zAliasWrite ? 1 : 0;
    u.uBlendScale = 1.0f / 128.0f;
    u.uTme = s.prim.tme ? 1 : 0;
    u.uFst = s.prim.fst ? 1 : 0;
    u.uIip = s.prim.iip ? 1 : 0;
    u.uTfx = c.tex0.tfx;
    u.uTcc = c.tex0.tcc;
    u.uTa0 = static_cast<float>(s.texa.ta0);
    u.uTa1 = static_cast<float>(s.texa.ta1);
    u.uAem = s.texa.aem ? 1 : 0;

    // texture selection (mirrors the desktop renderer)
    struct Views
    {
        VkImageView texView = VK_NULL_HANDLE;
        VkSampler texSampler = VK_NULL_HANDLE;
        VkImageView rtColorView = VK_NULL_HANDLE;
        VkImageView paletteView = VK_NULL_HANDLE;
        VkImageView idxPaletteView = VK_NULL_HANDLE;
        bool indexed = false;
        float texSize[2] = {1.f, 1.f};
        float texDim[2] = {1.f, 1.f};
    } vw;
    vw.texSampler = nearestSampler;
    vw.texView = whiteTex.view;
    vw.rtColorView = blackTex.view;
    vw.paletteView = blackTex.view;
    vw.idxPaletteView = blackTex.view;
    u.uTexSize[0] = u.uTexSize[1] = 1.0f;
    u.uTexDim[0] = static_cast<float>(std::max<uint16_t>(s.textureWidth, 1));
    u.uTexDim[1] = static_cast<float>(std::max<uint16_t>(s.textureHeight, 1));

    const auto family = [](uint8_t psm) { return (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S) ? 16 : (psm == GS_PSM_CT32 || psm == GS_PSM_CT24) ? 32 : 0; };
    RenderTarget *src = nullptr;
    RenderTarget *zsrc = nullptr;
    RenderTarget *rsrc = nullptr;
    if (s.prim.tme)
    {
        if (family(c.tex0.psm) != 0 && (c.tex0.tbp0 % 32u) == 0u)
            if (RenderTarget *candidate = findTarget(c.tex0.tbp0 / 32u))
            {
                const bool since = [this, candidate, tbp = c.tex0.tbp0]()
                {
                    for (const UploadRecord &rec : uploadLog)
                        if (rec.seq > candidate->lastDrawSeq && rec.lo <= tbp && tbp < rec.hi)
                            return true;
                    return false;
                }();
                bool depthOver = false;
                for (const RenderTarget &o : targets)
                    if (&o != candidate && o.zbp != ~0u && o.zbp == candidate->fbp && o.lastDepthSeq > candidate->lastDrawSeq)
                        depthOver = true;
                if (family(candidate->psm) == family(c.tex0.psm) && !since && !depthOver)
                    src = candidate;
            }
        const bool zFormat = c.tex0.psm == 0x30 || c.tex0.psm == 0x31 || c.tex0.psm == 0x32 || c.tex0.psm == 0x3A;
        if (zFormat && (c.tex0.tbp0 % 32u) == 0u)
            for (RenderTarget &t : targets)
                if (t.zbp != ~0u && t.zbp == c.tex0.tbp0 / 32u && t.fbp != t.zbp)
                    zsrc = &t;
        if (zsrc)
            src = nullptr;
        if (!src && !zsrc)
        {
            const bool highByte = c.tex0.psm == GS_PSM_T8H || c.tex0.psm == GS_PSM_T4HL || c.tex0.psm == GS_PSM_T4HH;
            for (RenderTarget &t : targets)
            {
                if (t.usedW == 0u || (highByte && t.psm == GS_PSM_CT24))
                    continue;
                bool depthOver = false;
                for (const RenderTarget &o : targets)
                    if (&o != &t && o.zbp != ~0u && o.zbp == t.fbp && o.lastDepthSeq > t.lastDrawSeq)
                        depthOver = true;
                if (depthOver)
                    continue;
                const uint32_t tlo = t.fbp * 32u, thi = tlo + spanBlocks(t.psm, t.fbw, t.usedH);
                if (c.tex0.tbp0 < tlo || c.tex0.tbp0 >= thi)
                    continue;
                bool since = false;
                for (const UploadRecord &rec : uploadLog)
                    if (rec.seq > t.lastDrawSeq && rec.lo <= c.tex0.tbp0 && c.tex0.tbp0 < rec.hi)
                        since = true;
                if (since)
                    continue;
                rsrc = &t;
                break;
            }
        }
        if (src)
        {
            vw.texView = src->colorSampled;
            vw.texSampler = s.linearFilter ? linearSampler : nearestSampler;
            u.uTexSize[0] = static_cast<float>(src->pxW);
            u.uTexSize[1] = static_cast<float>(src->pxH);
            u.uRtClamp[0] = (c.clamp & 3u) == 1u ? 1.0f : 0.0f;
            u.uRtClamp[1] = ((c.clamp >> 2) & 3u) == 1u ? 1.0f : 0.0f;
            u.uRtTex24 = c.tex0.psm == GS_PSM_CT24 ? 1 : family16(c.tex0.psm) ? 2 : 0;
        }
        else if (zsrc)
        {
            vw.texView = zsrc->depthSampled;
            u.uZTex = c.tex0.psm == 0x32 || c.tex0.psm == 0x3A ? 16 : c.tex0.psm == 0x31 ? 24 : 32;
            u.uZMax = zMaxFor(static_cast<uint8_t>(c.tex0.psm));
            u.uTexSize[0] = static_cast<float>(zsrc->pxW);
            u.uTexSize[1] = static_cast<float>(zsrc->pxH);
        }
        else if (rsrc)
        {
            vw.texView = rsrc->colorSampled;
            u.uReinterp = 1;
            u.uRtTex24 = c.tex0.psm == GS_PSM_CT24 ? 1 : family16(c.tex0.psm) ? 2 : 0;
            u.uTexSize[0] = static_cast<float>(rsrc->pxW);
            u.uTexSize[1] = static_cast<float>(rsrc->pxH);
            const GSSwizzle::Format &tf = GSSwizzle::GetFormat(c.tex0.psm);
            const GSSwizzle::Format &rf = GSSwizzle::GetFormat(rsrc->psm);
            int kind = 3, texelBits = 8;
            switch (tf.kind)
            {
            case GSSwizzle::Kind::Bits32: kind = 0; texelBits = 32; break;
            case GSSwizzle::Kind::Bits24: kind = 1; texelBits = 24; break;
            case GSSwizzle::Kind::Bits16: kind = 2; texelBits = 16; break;
            case GSSwizzle::Kind::Bits8: kind = 3; texelBits = 8; break;
            default: kind = 3; texelBits = 4; break;
            }
            u.uTexFmt[0] = tf.pageShiftX; u.uTexFmt[1] = tf.pageShiftY; u.uTexFmt[2] = tf.pixelsPerPageShift; u.uTexFmt[3] = tf.unpackedShift;
            u.uTexFmt2[0] = tf.bitOffset; u.uTexFmt2[1] = texelBits;
            u.uTexFmt2[2] = static_cast<int32_t>(tableOffset[tf.table]);
            u.uTexFmt2[3] = kind;
            u.uRtFmt[0] = rf.pageShiftX; u.uRtFmt[1] = rf.pageShiftY; u.uRtFmt[2] = rf.pixelsPerPageShift; u.uRtFmt[3] = 1 << rf.unpackedShift;
            u.uRtFmt2[0] = static_cast<int32_t>(inverseOffset[rsrc->psm]);
            u.uRtFmt2[1] = static_cast<int32_t>(rsrc->fbp);
            u.uRtFmt2[2] = static_cast<int32_t>(rsrc->fbw);
            u.uTex0[0] = static_cast<int32_t>(c.tex0.tbp0);
            u.uTex0[1] = c.tex0.tbw;
            u.uTex0[2] = std::max<uint16_t>(s.textureWidth, 1);
            u.uTex0[3] = std::max<uint16_t>(s.textureHeight, 1);
            u.uRtPx[0] = static_cast<float>(rsrc->pxW);
            u.uRtPx[1] = static_cast<float>(rsrc->pxH);
            vw.rtColorView = rsrc->colorSampled;
            if (kind == 3)
            {
                uploadPaletteImage(s);
                vw.paletteView = paletteTex.view;
            }
        }
        else
        {
            const bool indexed = isIndexedPsm(c.tex0.psm) && vram != nullptr;
            u.uIndexed = indexed ? 1 : 0;
            u.uLinear = s.linearFilter ? 1 : 0;
            if (indexed)
            {
                Texture &idx = indexTextureFor(s);
                vw.texView = idx.view;
                vw.texSampler = nearestSampler;
                Texture &pal = paletteFor(s);
                vw.idxPaletteView = pal.view;
                u.uWrap[0] = (c.clamp & 3u) == 0u ? 0 : 1;
                u.uWrap[1] = ((c.clamp >> 2) & 3u) == 0u ? 0 : 1;
            }
            else
            {
                Texture &tex = textureFor(s);
                vw.texView = tex.view;
                vw.texSampler = s.linearFilter ? linearSampler : nearestSampler;
            }
            u.uTexSize[0] = static_cast<float>(std::max<uint16_t>(s.textureWidth, 1));
            u.uTexSize[1] = static_cast<float>(std::max<uint16_t>(s.textureHeight, 1));
        }
    }

    // a failed texture allocation must not reach the descriptor set
    if (!vw.texView)
        vw.texView = whiteTex.view;
    if (!vw.idxPaletteView)
        vw.idxPaletteView = blackTex.view;
    if (!vw.paletteView)
        vw.paletteView = blackTex.view;
    if (!vw.rtColorView)
        vw.rtColorView = blackTex.view;

    const uint32_t ate = (c.test & 1u) != 0u ? 1u : 0u;
    const uint32_t atst = static_cast<uint32_t>((c.test >> 1) & 7u), aref = static_cast<uint32_t>((c.test >> 4) & 0xFFu);
    const uint32_t afail = static_cast<uint32_t>((c.test >> 12) & 3u);
    u.uAte = ate;
    u.uAtst = static_cast<int32_t>(atst);
    u.uAref = static_cast<int32_t>(aref);
    u.uPass = ate ? 1 : 0;

    // vertices + UBO in the ring (final offsets before the descriptors are written)
    const VkDeviceSize vbytes = dverts.size() * sizeof(HwVertex);
    auto valloc = rings.alloc(vbytes, 16);
    auto ubo = rings.alloc(sizeof(DrawUbo), 256);
    if (!valloc.cpu || !ubo.cpu)
    {
        submitAndWait();
        rings.reset();
        staging.reset();
        valloc = rings.alloc(vbytes, 16);
        ubo = rings.alloc(sizeof(DrawUbo), 256);
        if (!valloc.cpu || !ubo.cpu)
            return;
    }
    std::memcpy(valloc.cpu, dverts.data(), vbytes);
    std::memcpy(ubo.cpu, &u, sizeof(u));

    VkDescriptorBufferInfo bufInfo{rings.buffer, ubo.offset, sizeof(DrawUbo)};
    VkDescriptorImageInfo texInfo{vw.texSampler, vw.texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo destInfo{VK_NULL_HANDLE, rt.colorInput, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo tablesInfo{nearestSampler, tableView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo rtColorInfo{nearestSampler, vw.rtColorView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo palInfo{nearestSampler, vw.paletteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo idxPalInfo{nearestSampler, vw.idxPaletteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[7] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &texInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 1, 0, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &destInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &tablesInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &rtColorInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &palInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &idxPalInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 6, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufInfo, nullptr},
    };
    gs_vk::vk.UpdateDescriptorSets(device, 7, writes, 0, nullptr);

    // vertex Z compare
    uint32_t depthCompare = VK_COMPARE_OP_ALWAYS;
    if (!zAlias)
    {
        depthCompare = ztst == 0 ? VK_COMPARE_OP_NEVER : ztst == 1 ? VK_COMPARE_OP_ALWAYS : ztst == 2 ? (zInv ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL) : (zInv ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_GREATER);
    }

    const auto makeKey = [&](bool blendEnable, uint32_t sfk, uint32_t dfk, uint32_t eq, bool depthWrite, uint32_t wmask, bool secondPass)
    {
        PipelineKey key{};
        key.program = Program::Draw;
        key.blendEnable = blendEnable && !secondPass ? 1u : 0u;
        key.srcColor = sfk;
        key.dstColor = dfk;
        key.srcAlpha = VK_BLEND_FACTOR_ONE;
        key.dstAlpha = VK_BLEND_FACTOR_ZERO;
        key.equation = eq;
        key.depthTest = zAlias ? 0u : 1u; // ZTE=0 is no depth test on the GS: ALWAYS (a write still lands)
        key.depthWrite = depthWrite && !zAlias ? 1u : 0u;
        key.depthCompare = key.depthTest ? depthCompare : VK_COMPARE_OP_ALWAYS;
        key.writeMask = wmask;
        return key;
    };

    // vertices: a GS line quad and sprite quads are all emitted as triangles
    VkDeviceSize voff = valloc.offset;
    const auto recordPass = [&](bool pass2)
    {
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = rpDraw;
        rb.framebuffer = rt.fb;
        rb.renderArea.extent = {rt.pxW, rt.pxH};
        gs_vk::vk.CmdBeginRenderPass(frameCb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(rt.pxW), static_cast<float>(rt.pxH), 0.0f, 1.0f};
        VkRect2D scissor{{static_cast<int32_t>(c.scissor.x0), static_cast<int32_t>(c.scissor.y0)},
                         {std::max(1u, c.scissor.x1 - c.scissor.x0 + 1u), std::max(1u, c.scissor.y1 - c.scissor.y0 + 1u)}};
        gs_vk::vk.CmdSetViewport(frameCb, 0, 1, &viewport);
        gs_vk::vk.CmdSetScissor(frameCb, 0, 1, &scissor);
        gs_vk::vk.CmdBindDescriptorSets(frameCb, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipelineLayout, 0, 1, &dsc, 0, nullptr);
        VkBuffer vb = rings.buffer;
        VkDeviceSize offs = voff;
        gs_vk::vk.CmdBindVertexBuffers(frameCb, 0, 1, &vb, &offs);
        // pass 1 (uPass=1): keep passing fragments. pass 2 (AFAIL): keep failing ones
        u.uPass = pass2 ? 2 : (ate ? 1 : 0);
        std::memcpy(ubo.cpu, &u, sizeof(u));
                gs_vk::vk.CmdBindPipeline(frameCb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(makeKey(blend, sf, df, equation, zwrite && !pass2, writeMask, pass2)));
        if (softBlend && !zAliasWrite && (s.colclamp & 1u) == 0u)
        {
            // the shader reads the destination: one triangle at a time so overlapping triangles of one draw see each
            // other's results like on the GS (the shadow volumes count overlaps this way)
            for (size_t i = 0; i + 3 <= dverts.size(); i += 3)
            {
                if (i != 0)
                    globalBarrier();
                gs_vk::vk.CmdDraw(frameCb, 3, 1, static_cast<uint32_t>(i), 0);
            }
        }
        else
                    gs_vk::vk.CmdDraw(frameCb, static_cast<uint32_t>(dverts.size()), 1, 0, 0);
        gs_vk::vk.CmdEndRenderPass(frameCb);
    };
    recordPass(false);
    if (ate && afail != 0u)
    {
        uint32_t failMask = writeMask;
        bool failDepthWrite = zwrite;
        if (afail == 1u) failDepthWrite = false;                                   // FB_ONLY: colour+alpha, no Z
        else if (afail == 2u) failMask = 0u;                                       // ZB_ONLY: Z only
        else { failDepthWrite = false; failMask = writeMask & ~VK_COLOR_COMPONENT_A_BIT; } // RGB_ONLY
        u.uPass = 2;
        std::memcpy(ubo.cpu, &u, sizeof(u));
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = rpDraw;
        rb.framebuffer = rt.fb;
        rb.renderArea.extent = {rt.pxW, rt.pxH};
        gs_vk::vk.CmdBeginRenderPass(frameCb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(rt.pxW), static_cast<float>(rt.pxH), 0.0f, 1.0f};
        VkRect2D scissor{{static_cast<int32_t>(c.scissor.x0), static_cast<int32_t>(c.scissor.y0)},
                         {std::max(1u, c.scissor.x1 - c.scissor.x0 + 1u), std::max(1u, c.scissor.y1 - c.scissor.y0 + 1u)}};
        gs_vk::vk.CmdSetViewport(frameCb, 0, 1, &viewport);
        gs_vk::vk.CmdSetScissor(frameCb, 0, 1, &scissor);
        gs_vk::vk.CmdBindDescriptorSets(frameCb, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipelineLayout, 0, 1, &dsc, 0, nullptr);
        VkBuffer vb = rings.buffer;
        VkDeviceSize offs = voff;
        gs_vk::vk.CmdBindVertexBuffers(frameCb, 0, 1, &vb, &offs);
        gs_vk::vk.CmdBindPipeline(frameCb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipeline(makeKey(false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, failDepthWrite, failMask, true)));
        gs_vk::vk.CmdDraw(frameCb, static_cast<uint32_t>(dverts.size()), 1, 0, 0);
        gs_vk::vk.CmdEndRenderPass(frameCb);
    }
    globalBarrier();
}

// ==== channel shuffles (see shaders/shuffle.*) =====================================================

const std::vector<uint16_t> &GSVkBackend::Impl::inverseFor(uint32_t ownerPsm)
{
    std::vector<uint16_t> &inv = ownerInverse[ownerPsm];
    if (inv.empty())
    {
        const GSSwizzle::Format &f = GSSwizzle::GetFormat(ownerPsm);
        inv.assign(2048u, 0);
        for (uint32_t e = 0; e < 2048u; ++e)
            inv[f.table[e]] = static_cast<uint16_t>(e);
    }
    return inv;
}

// pixel (x, y) of a 16 bit view (psm/base/bw) -> pixel and halfword of the 32 bit owner laid out as ownerPsm at fbp/fbw
bool GSVkBackend::Impl::viewToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm, uint32_t fbp, uint32_t fbw,
                                    uint32_t &ox, uint32_t &oy, uint32_t &half)
{
    const uint32_t byte = GSSwizzle::Locate(GSSwizzle::GetFormat(viewPsm), base, bw, x, y).byte;
    const uint32_t word = byte >> 2, first = fbp * 2048u;
    if (word < first)
        return false;
    const uint32_t rel = word - first, page = rel >> 11u, e = inverseFor(ownerPsm)[rel & 2047u];
    half = (byte >> 1) & 1u;
    ox = (page % fbw) * 64u + (e & 63u);
    oy = (page / fbw) * 32u + (e >> 6u);
    return true;
}

// an 8x8 unit of the view at (x, y) -> an 8x8 owner block at (ox, oy), one halfword (checked at the far corner)
bool GSVkBackend::Impl::unitToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm, uint32_t fbp, uint32_t fbw,
                                    uint32_t &ox, uint32_t &oy, uint32_t &half)
{
    uint32_t ex, ey, eh;
    return viewToOwner(viewPsm, base, bw, x, y, ownerPsm, fbp, fbw, ox, oy, half) &&
           viewToOwner(viewPsm, base, bw, x + 7u, y + 7u, ownerPsm, fbp, fbw, ex, ey, eh) && eh == half && ex == ox + 7u && ey == oy + 7u;
}

// a sprite drawn through a 16 bit view of a 32 bit target, textured from a 16 bit view of a 32 bit target or a Z buffer,
// 8 px aligned, copying texels 1:1: queued as a channel shuffle. False = not that shape (the caller decides).
bool GSVkBackend::Impl::tryShuffle(const GSDrawState &s, const GSPrimitiveBatch &b)
{
    const GSContext &c = s.context;
    const uint8_t tpsm = c.tex0.psm;
    const bool zTex = tpsm == 0x32 || tpsm == 0x3A;
    if (s.prim.type != GS_PRIM_SPRITE || !s.prim.tme || !s.prim.fst || s.prim.abe || !(family16(tpsm) || zTex))
        return false;
    if ((c.test & 1u) != 0u && ((c.test >> 1) & 7u) != 1u)
        return false;
    if (((c.test >> 14) & 1u) != 0u)
        return false;
    {
        const GSVertex &v = b.vertices[1];
        const bool identity = c.tex0.tfx == 1 || (c.tex0.tfx == 0 && v.r == 128 && v.g == 128 && v.b == 128 && (!c.tex0.tcc || v.a == 128));
        if (!identity)
            return false;
    }
    const uint32_t m16 = fbmsk16(c.frame.fbmsk);
    const uint32_t lowByte = m16 & 0xFFu, highByte = m16 & 0xFF00u;
    if ((lowByte != 0u && lowByte != 0xFFu) || (highByte != 0u && highByte != 0xFF00u) || m16 == 0xFFFFu)
        return false;
    RenderTarget *dst = findTarget(c.frame.fbp);
    if (dst && family16(dst->psm) && dst->usedW != 0u)
        return false;
    if ((c.tex0.tbp0 % 32u) != 0u)
        return false;
    RenderTarget *src = nullptr;
    bool srcDepth = false;
    if (zTex)
        for (RenderTarget &t : targets)
            if (t.zbp != ~0u && t.zbp == c.tex0.tbp0 / 32u && t.fbp != t.zbp)
            {
                const bool widthMatches = t.fbw == c.tex0.tbw;
                const bool srcWidthMatches = src && src->fbw == c.tex0.tbw;
                if (!src || (widthMatches && !srcWidthMatches) ||
                    (widthMatches == srcWidthMatches && t.lastDepthSeq >= src->lastDepthSeq))
                {
                    src = &t;
                    srcDepth = true;
                }
            }
    if (!src)
        if (RenderTarget *t = findTarget(c.tex0.tbp0 / 32u))
            if (!family16(t->psm) && t->usedW != 0u)
                src = t;
    if (!src)
        return false;
    const GSVertex &a = b.vertices[0], &e = b.vertices[1];
    const float ofx = c.xyoffset.ofx / 16.0f, ofy = c.xyoffset.ofy / 16.0f;
    const float fx0 = std::min(a.x, e.x) - ofx, fx1 = std::max(a.x, e.x) - ofx, fy0 = std::min(a.y, e.y) - ofy, fy1 = std::max(a.y, e.y) - ofy;
    const auto whole = [](float v) { return std::fabs(v - std::round(v)) < 1e-3f && v >= 0.0f; };
    if (!whole(fx0) || !whole(fy0) || fx1 < 0.0f || fy1 < 0.0f)
        return false;
    const int x0 = static_cast<int>(std::lround(fx0)), x1 = static_cast<int>(std::ceil(fx1 - 1e-3f));
    const int y0 = static_cast<int>(std::lround(fy0)), y1 = static_cast<int>(std::ceil(fy1 - 1e-3f));
    const GSVertex &left = (a.x <= e.x) ? a : e;
    const GSVertex &top = (a.y <= e.y) ? a : e;
    const int du = static_cast<int>(std::floor(left.u / 16.0f)) - x0, dv = static_cast<int>(std::floor(top.v / 16.0f)) - y0;
    const float uSpan = std::fabs(static_cast<float>(a.u) - static_cast<float>(e.u)) / 16.0f;
    const float vSpan = std::fabs(static_cast<float>(a.v) - static_cast<float>(e.v)) / 16.0f;
    if (((x0 | x1 | y0 | y1 | du | dv) & 7) != 0 || x1 <= x0 || y1 <= y0 || std::fabs(uSpan - (fx1 - fx0)) > 1e-2f ||
        std::fabs(vSpan - (fy1 - fy0)) > 1e-2f)
        return false;
    if (x0 + du < 0 || y0 + dv < 0 || x0 < static_cast<int>(c.scissor.x0) || y0 < static_cast<int>(c.scissor.y0) ||
        x1 - 1 > static_cast<int>(c.scissor.x1) || y1 - 1 > static_cast<int>(c.scissor.y1))
        return false;
    if (!dst)
    {
        flushShuffle();
        const size_t srcIndex = static_cast<size_t>(src - targets.data());
        dst = &target(c.frame.fbp, c.frame.fbw);
        src = &targets[srcIndex];
    }
    const uint32_t srcOwnerPsm = srcDepth ? 0x30u : 0x00u, srcFbp = srcDepth ? src->zbp : src->fbp;
    std::vector<ShuffleVertex> quads[4];
    uint32_t maxX = 0, maxY = 0;
    for (int y = y0; y < y1; y += 8)
        for (int x = x0; x < x1; x += 8)
        {
            uint32_t dx, dy, hd, sx, sy, hs;
            if (!unitToOwner(c.frame.psm, c.frame.fbp * 32u, c.frame.fbw, x, y, 0x00u, dst->fbp, dst->fbw, dx, dy, hd) ||
                !unitToOwner(tpsm, c.tex0.tbp0, c.tex0.tbw, x + du, y + dv, srcOwnerPsm, srcFbp, src->fbw, sx, sy, hs))
                return false;
            if (dx + 8u > dst->pxW || dy + 8u > dst->pxH || sx + 8u > src->pxW || sy + 8u > src->pxH)
                return false;
            if (src == dst && !srcDepth && (sx != dx || sy != dy))
                return false; // would read texels that other quads of the pass write
            const float ox = static_cast<float>(sx) - static_cast<float>(dx), oy = static_cast<float>(sy) - static_cast<float>(dy);
            const float X0 = static_cast<float>(dx), Y0 = static_cast<float>(dy), X1 = X0 + 8.0f, Y1 = Y0 + 8.0f;
            std::vector<ShuffleVertex> &q = quads[hs * 2u + hd];
            q.insert(q.end(), {{X0, Y0, ox, oy}, {X1, Y0, ox, oy}, {X0, Y1, ox, oy}, {X1, Y0, ox, oy}, {X1, Y1, ox, oy}, {X0, Y1, ox, oy}});
            maxX = std::max(maxX, dx + 8u);
            maxY = std::max(maxY, dy + 8u);
        }
    if (havePending)
        flushPending();
    if (haveShuffle && (std::memcmp(&shuffleState, &s, sizeof(GSDrawState)) != 0 || shuffleDst != dst || shuffleSrc != src || shuffleSrcZ != srcDepth))
        flushShuffle();
    if (!haveShuffle)
    {
        haveShuffle = true;
        shuffleState = s;
        shuffleDst = dst;
        shuffleSrc = src;
        shuffleSrcZ = srcDepth;
    }
    for (int i = 0; i < 4; ++i)
        shuffleQuads[i].insert(shuffleQuads[i].end(), quads[i].begin(), quads[i].end());
    dst->usedW = std::max(dst->usedW, std::min(maxX, dst->pxW));
    dst->usedH = std::max(dst->usedH, std::min(maxY, dst->pxH));
    return true;
}

void GSVkBackend::Impl::flushShuffle()
{
    if (!haveShuffle)
        return;
    haveShuffle = false;
    RenderTarget &dst = *shuffleDst, &src = *shuffleSrc;
    const GSContext &c = shuffleState.context;
    ++dst.version;
    dst.lastDrawSeq = ++writeSeq;
    const uint32_t m16 = fbmsk16(c.frame.fbmsk);
    const uint32_t lo = (m16 & 0xFFu) == 0u ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT) : 0u;
    const uint32_t hi = (m16 & 0xFF00u) == 0u ? (VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT) : 0u;

    beginFrameCb();
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &drawSetLayout;
    VkDescriptorSet dsc = VK_NULL_HANDLE;
    if (gs_vk::vk.AllocateDescriptorSets(device, &dai, &dsc) != VK_SUCCESS)
    {
        submitAndWait();
        gs_vk::vk.AllocateDescriptorSets(device, &dai, &dsc);
    }
    VkImageView srcView = shuffleSrcZ ? src.depthSampled : src.colorSampled;
    VkDescriptorImageInfo srcInfo{nearestSampler, srcView, shuffleSrcZ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo destInfo{VK_NULL_HANDLE, dst.colorInput, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo tablesInfo{nearestSampler, tableView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo rtColorInfo{nearestSampler, blackTex.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo palInfo{nearestSampler, blackTex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo idxPalInfo{nearestSampler, blackTex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    for (uint32_t i = 0; i < 4; ++i)
    {
        std::vector<ShuffleVertex> &q = shuffleQuads[i];
        if (q.empty())
            continue;
        const uint32_t hs = i >> 1, hd = i & 1u;
        auto ubo = rings.alloc(sizeof(ShuffleUbo), 256);
        if (!ubo.cpu)
        {
            submitAndWait();
            rings.reset();
            staging.reset();
            ubo = rings.alloc(sizeof(ShuffleUbo), 256);
        }
        ShuffleUbo u{};
        u.uInvSize[0] = 1.0f / dst.pxW;
        u.uInvSize[1] = 1.0f / dst.pxH;
        u.uZMax = zMaxFor(c.zbuf.psm);
        u.uSrcZ = shuffleSrcZ ? 1 : 0;
        u.uHs = static_cast<int32_t>(hs);
        u.uHd = static_cast<int32_t>(hd);
        u.uScale = kScale;
        u.uTexa[0] = shuffleState.texa.ta0 >> 7;
        u.uTexa[1] = shuffleState.texa.ta1 >> 7;
        u.uTexa[2] = shuffleState.texa.aem ? 1 : 0;
        std::memcpy(ubo.cpu, &u, sizeof(u));
        VkDescriptorBufferInfo bufInfo{rings.buffer, ubo.offset, sizeof(ShuffleUbo)};
        VkDescriptorImageInfo texInfo{nearestSampler, srcView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[7] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &texInfo, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 1, 0, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &destInfo, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &tablesInfo, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &rtColorInfo, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &palInfo, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &idxPalInfo, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 6, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufInfo, nullptr},
        };
        gs_vk::vk.UpdateDescriptorSets(device, 7, writes, 0, nullptr);

        const VkDeviceSize vbytes = q.size() * sizeof(ShuffleVertex);
        auto valloc = rings.alloc(vbytes, 16);
        if (!valloc.cpu)
        {
            submitAndWait();
            rings.reset();
            staging.reset();
            valloc = rings.alloc(vbytes, 16);
            if (!valloc.cpu)
            {
                q.clear();
                continue;
            }
        }
        std::memcpy(valloc.cpu, q.data(), vbytes);
        const uint32_t vertexCount = static_cast<uint32_t>(q.size());
        q.clear();

        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = rpDraw;
        rb.framebuffer = dst.fb;
        rb.renderArea.extent = {dst.pxW, dst.pxH};
        gs_vk::vk.CmdBeginRenderPass(frameCb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(dst.pxW), static_cast<float>(dst.pxH), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {dst.pxW, dst.pxH}};
        gs_vk::vk.CmdSetViewport(frameCb, 0, 1, &viewport);
        gs_vk::vk.CmdSetScissor(frameCb, 0, 1, &scissor);
        gs_vk::vk.CmdBindDescriptorSets(frameCb, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipelineLayout, 0, 1, &dsc, 0, nullptr);
        VkBuffer vb = rings.buffer;
        VkDeviceSize offs = valloc.offset;
        gs_vk::vk.CmdBindVertexBuffers(frameCb, 0, 1, &vb, &offs);
        PipelineKey key{};
        key.program = Program::Shuffle;
        key.depthTest = 0;
        key.depthCompare = VK_COMPARE_OP_ALWAYS;
        key.writeMask = (hd == 0u ? lo : 0u) | (hd == 1u ? hi : 0u);
        key.srcAlpha = key.dstAlpha = VK_BLEND_FACTOR_ONE;
        gs_vk::vk.CmdBindPipeline(frameCb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(key));
        gs_vk::vk.CmdDraw(frameCb, vertexCount, 1, 0, 0);
        gs_vk::vk.CmdEndRenderPass(frameCb);
        globalBarrier();
    }
    (void)srcInfo;
    (void)destInfo;
    (void)tablesInfo;
    (void)rtColorInfo;
    (void)palInfo;
    (void)idxPalInfo;
}

// ==== presentation =================================================================================

void GSVkBackend::Impl::uploadPaletteImage(const GSDrawState &s)
{
    uint32_t colors[256];
    store.BuildPalette(s, colors);
    // the shared palette image is updated before each reinterpret draw that needs it
    beginFrameCb();
    auto a = staging.alloc(256 * 4, 16);
    if (!a.cpu)
    {
        submitAndWait();
        staging.reset();
        a = staging.alloc(256 * 4, 16);
    }
    std::memcpy(a.cpu, colors, 256 * 4);
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.image = paletteTex.image;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    gs_vk::vk.CmdPipelineBarrier(frameCb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {256, 1, 1};
    gs_vk::vk.CmdCopyBufferToImage(frameCb, staging.buffer, paletteTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    VkImageMemoryBarrier bar2 = bar;
    bar2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    gs_vk::vk.CmdPipelineBarrier(frameCb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar2);
}

void GSVkBackend::Impl::ensurePresentImage(uint32_t w, uint32_t h)
{
    if (presentW == w && presentH == h && presentImage)
        return;
    flushPending();
    submitAndWait();
    if (presentFb) gs_vk::vk.DestroyFramebuffer(device, presentFb, nullptr);
    if (presentView) gs_vk::vk.DestroyImageView(device, presentView, nullptr);
    if (presentImage) gs_vk::vk.DestroyImage(device, presentImage, nullptr);
    if (presentMem) gs_vk::vk.FreeMemory(device, presentMem, nullptr);
    presentImage = VK_NULL_HANDLE;
presentView = VK_NULL_HANDLE;
presentFb = VK_NULL_HANDLE;
    presentMem = VK_NULL_HANDLE;
    presentW = presentH = 0;

    VkImageCreateInfo imi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imi.imageType = VK_IMAGE_TYPE_2D;
    imi.format = VK_FORMAT_R8G8B8A8_UNORM;
    imi.extent = {w, h, 1};
    imi.mipLevels = imi.arrayLayers = 1;
    imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (gs_vk::vk.CreateImage(device, &imi, nullptr, &presentImage) != VK_SUCCESS)
        return;
    VkMemoryRequirements req{};
    gs_vk::vk.GetImageMemoryRequirements(device, presentImage, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (gs_vk::vk.AllocateMemory(device, &ai, nullptr, &presentMem) != VK_SUCCESS)
        return;
    gs_vk::vk.BindImageMemory(device, presentImage, presentMem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = presentImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    gs_vk::vk.CreateImageView(device, &vi, nullptr, &presentView);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = rpPresent;
    fi.attachmentCount = 1;
    fi.pAttachments = &presentView;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    gs_vk::vk.CreateFramebuffer(device, &fi, nullptr, &presentFb);
    presentW = w;
    presentH = h;
}

void GSVkBackend::Impl::drawCircuit(VkImageView src, uint32_t cdbx, uint32_t cdby, bool ct24, bool alphaFactor, uint32_t srcEdge, bool blendOver,
                                    uint32_t pmode, bool mmod, uint32_t alp)
{
    (void)pmode;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &drawSetLayout;
    VkDescriptorSet dsc = VK_NULL_HANDLE;
    gs_vk::vk.AllocateDescriptorSets(device, &dai, &dsc);
    auto ubo = rings.alloc(sizeof(PresentUbo), 256);
    if (!ubo.cpu)
    {
        submitAndWait();
        rings.reset();
        staging.reset();
        ubo = rings.alloc(sizeof(PresentUbo), 256);
        gs_vk::vk.AllocateDescriptorSets(device, &dai, &dsc);
    }
    PresentUbo u{};
    u.uSrcOffset[0] = static_cast<int32_t>(cdbx * kScale);
    u.uSrcOffset[1] = static_cast<int32_t>(cdby * kScale);
    u.uSrcOffset[2] = ct24 ? 1 : 0;
    u.uSrcOffset[3] = alphaFactor ? 1 : 0;
    u.uSrcEdge[0] = static_cast<int32_t>(srcEdge);
    std::memcpy(ubo.cpu, &u, sizeof(u));
    VkDescriptorImageInfo texInfo{nearestSampler, src, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo destInfo{VK_NULL_HANDLE, presentView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo tablesInfo{nearestSampler, tableView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo rtColorInfo{nearestSampler, blackTex.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo palInfo{nearestSampler, blackTex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo idxPalInfo{nearestSampler, blackTex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bufInfo{rings.buffer, ubo.offset, sizeof(PresentUbo)};
    VkWriteDescriptorSet writes[7] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &texInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 1, 0, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &destInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &tablesInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &rtColorInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &palInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &idxPalInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsc, 6, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufInfo, nullptr},
    };
    gs_vk::vk.UpdateDescriptorSets(device, 7, writes, 0, nullptr);

    PipelineKey key{};
    key.program = Program::Present;
    key.writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    key.srcAlpha = key.dstAlpha = VK_BLEND_FACTOR_ONE;
    if (blendOver)
    {
        key.blendEnable = 1;
        if (mmod)
        {
            key.srcColor = VK_BLEND_FACTOR_CONSTANT_COLOR;
            key.dstColor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            key.srcAlpha = VK_BLEND_FACTOR_ONE;
            key.dstAlpha = VK_BLEND_FACTOR_ZERO;
        }
        else
        {
            key.srcColor = VK_BLEND_FACTOR_SRC_ALPHA;
            key.dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            key.srcAlpha = VK_BLEND_FACTOR_ONE;
            key.dstAlpha = VK_BLEND_FACTOR_ZERO;
        }
    }
    if (blendOver && mmod)
    {
        const float f = static_cast<float>(alp) / 255.f;
        const float cc[4] = {f, f, f, f};
        gs_vk::vk.CmdSetBlendConstants(frameCb, cc);
    }
    gs_vk::vk.CmdBindPipeline(frameCb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(key));
    gs_vk::vk.CmdBindDescriptorSets(frameCb, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipelineLayout, 0, 1, &dsc, 0, nullptr);
    gs_vk::vk.CmdDraw(frameCb, 3, 1, 0, 0);
}

void GSVkBackend::Impl::ensureSwapchain()
{
#ifdef __ANDROID__
    void *win = ps2x::host::NativeWindow();
    presentWindow = win;
    if (!win)
    {
        destroySwapchain();
        return;
    }
    int sw = 0, sh = 0;
    ps2x::host::GetSurfaceSize(sw, sh);
    if (swap.swapchain && static_cast<int>(swap.width) == sw && static_cast<int>(swap.height) == sh)
        return;
    destroySwapchain();
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = static_cast<ANativeWindow *>(win);
    if (gs_vk::vk.CreateAndroidSurfaceKHR(instance, &si, nullptr, &swap.surface) != VK_SUCCESS)
    {
        std::fprintf(stderr, "[gs-vk] surface creation failed\n");
        return;
    }
    VkSurfaceCapabilitiesKHR caps{};
    if (!gs_vk::vk.GetPhysicalDeviceSurfaceCapabilitiesKHR || gs_vk::vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(phys, swap.surface, &caps) != VK_SUCCESS)
        return;
    VkSurfaceFormatKHR formats[8] = {};
    uint32_t nf = 8;
    if (!gs_vk::vk.GetPhysicalDeviceSurfaceFormatsKHR || gs_vk::vk.GetPhysicalDeviceSurfaceFormatsKHR(phys, swap.surface, &nf, formats) != VK_SUCCESS || nf == 0)
        return;
    swap.format = (formats[0].format == VK_FORMAT_UNDEFINED) ? VK_FORMAT_R8G8B8A8_UNORM : formats[0].format;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = swap.surface;
    ci.minImageCount = std::max(2u, caps.minImageCount);
    ci.imageFormat = swap.format;
    ci.imageColorSpace = formats[0].colorSpace;
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    if (gs_vk::vk.CreateSwapchainKHR(device, &ci, nullptr, &swap.swapchain) != VK_SUCCESS)
    {
        std::fprintf(stderr, "[gs-vk] swapchain creation failed\n");
        return;
    }
    uint32_t n = 0;
    gs_vk::vk.GetSwapchainImagesKHR(device, swap.swapchain, &n, nullptr);
    swap.images.resize(n);
    gs_vk::vk.GetSwapchainImagesKHR(device, swap.swapchain, &n, swap.images.data());
    swap.width = caps.currentExtent.width;
    swap.height = caps.currentExtent.height;
    swap.ok = true;
    std::fprintf(stderr, "[gs-vk] swapchain %ux%u fmt=%d images=%zu\n", swap.width, swap.height, static_cast<int>(swap.format), swap.images.size());
#else
    presentWindow = nullptr;
#endif
}

void GSVkBackend::Impl::destroySwapchain()
{
    if (swap.swapchain) gs_vk::vk.DestroySwapchainKHR(device, swap.swapchain, nullptr);
    if (swap.surface) gs_vk::vk.DestroySurfaceKHR(instance, swap.surface, nullptr);
    swap.swapchain = VK_NULL_HANDLE;
    swap.surface = VK_NULL_HANDLE;
    swap.images.clear();
    swap.ok = false;
}

PresentationFrame GSVkBackend::Present(const GSPresentationRequest &request)
{
    Impl &d = *m_impl;
    PresentationFrame frame;
    if (d.failed || !d.ensure())
        return frame;
    d.flushPending();
    ++d.presentCount;
    const bool enable1 = (request.pmode & 1ull) != 0ull, enable2 = (request.pmode & 2ull) != 0ull;
    const auto decode = [&](bool enabled, uint64_t dispfb, uint64_t display, uint32_t &fbp, uint32_t &fbw, uint32_t &dbx, uint32_t &dby,
                            uint32_t &w, uint32_t &h)
    {
        fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        fbw = std::max<uint32_t>(1u, static_cast<uint32_t>((dispfb >> 9) & 0x3Fu));
        dbx = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
        dby = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
        const uint32_t hdiv = static_cast<uint32_t>((display >> 23) & 0xFu) + 1u;
        const uint32_t vdiv = static_cast<uint32_t>((display >> 27) & 0x3u) + 1u;
        w = (static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u) / hdiv;
        h = (static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u) / vdiv;
        return enabled && display != 0u && w != 0u && h != 0u;
    };
    uint32_t fbp1, fbw1, dbx1, dby1, w1, h1, fbp2, fbw2, dbx2, dby2, w2, h2;
    const bool ok1 = enable1 && decode(true, request.dispfb1, request.display1, fbp1, fbw1, dbx1, dby1, w1, h1);
    const bool ok2 = enable2 && decode(true, request.dispfb2, request.display2, fbp2, fbw2, dbx2, dby2, w2, h2);
    if (!ok1 && !ok2)
        return frame;
    constexpr uint32_t kTvLines = 448u;
    const uint32_t fbp = ok1 ? fbp1 : fbp2, w = ok1 ? w1 : w2, h = std::min(ok1 ? h1 : h2, kTvLines);
    frame.displayFbp = fbp;
    frame.sourceFbp = fbp;
    const uint32_t outW = std::min(w, 2048u), outH = std::min(h, 512u);
    frame.width = outW;
    frame.height = outH;
    frame.pixels.assign(static_cast<size_t>(outW) * outH * 4u, 0u);

    d.ensurePresentImage(outW, outH);
    if (!d.presentFb)
        return frame;

    d.beginFrameCb();
    {
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = d.rpPresent;
        rb.framebuffer = d.presentFb;
        rb.renderArea.extent = {outW, outH};
        VkClearValue clear{};
        clear.color = {{0.f, 0.f, 0.f, 1.f}};
        rb.pClearValues = &clear;
        rb.clearValueCount = 1;
        gs_vk::vk.CmdBeginRenderPass(d.frameCb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(outW), static_cast<float>(outH), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {outW, outH}};
        gs_vk::vk.CmdSetViewport(d.frameCb, 0, 1, &viewport);
        gs_vk::vk.CmdSetScissor(d.frameCb, 0, 1, &scissor);
        const uint32_t srcEdge = static_cast<uint32_t>(std::lround(2.0f * kScale));
        const bool mmod = ((request.pmode >> 5) & 1u) != 0u, slbg = ((request.pmode >> 7) & 1u) != 0u;
        const uint32_t alp = static_cast<uint32_t>((request.pmode >> 8) & 0xFFu);
        auto drawOne = [&](uint32_t cfbp, uint64_t dispfb, uint32_t cdbx, uint32_t cdby, bool alphaFactor, bool blendOver)
        {
            Impl::RenderTarget *crt = d.findTarget(cfbp);
            if (!crt)
                return;
            const bool ct24 = ((dispfb >> 15) & 0x1Fu) == GS_PSM_CT24;
            d.drawCircuit(crt->colorSampled, cdbx, cdby, ct24, alphaFactor, srcEdge, blendOver, 0, mmod, alp);
        };
        if (ok1 && ok2)
        {
            if (slbg)
            {
                // circuit 1 over the background colour
            }
            else
                drawOne(fbp2, request.dispfb2, dbx2, dby2, false, false);
            drawOne(fbp1, request.dispfb1, dbx1, dby1, true, true);
        }
        else
            drawOne(fbp, ok1 ? request.dispfb1 : request.dispfb2, ok1 ? dbx1 : dbx2, ok1 ? dby1 : dby2, false, false);
        gs_vk::vk.CmdEndRenderPass(d.frameCb);
        (void)slbg;
    }
    // readback for the latched frame
    {
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {outW, outH, 1};
        gs_vk::vk.CmdCopyImageToBuffer(d.frameCb, d.presentImage, VK_IMAGE_LAYOUT_GENERAL, d.readbackBuffer, 1, &cp);
    }
    d.endFrameCb();

    // submit and wait (one submission per present keeps everything simple)
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.commandBufferCount = 1;
    su.pCommandBuffers = &d.frameCb;
    gs_vk::vk.ResetFences(d.device, 1, &d.frameFence);
    gs_vk::vk.QueueSubmit(d.queue, 1, &su, d.frameFence);
    gs_vk::vk.WaitForFences(d.device, 1, &d.frameFence, VK_TRUE, 1000000000ull);
    gs_vk::vk.ResetCommandBuffer(d.frameCb, 0);
    gs_vk::vk.ResetCommandPool(d.device, d.cmdPool, 0);
    d.cbOpen = false;
    d.emptyTrash();

    std::memcpy(frame.pixels.data(), d.readbackMapped, frame.pixels.size());
    // the displayed image is opaque: force alpha to 1
    for (size_t i = 3; i < frame.pixels.size(); i += 4)
        frame.pixels[i] = 255u;
    // the frontend packs presentation rows with a fixed 640 pixel stride
    if (frame.width < 640u)
    {
        std::vector<uint8_t> rows(static_cast<size_t>(640u) * frame.height * 4u, 0u);
        for (uint32_t y = 0; y < frame.height; ++y)
            std::memcpy(rows.data() + static_cast<size_t>(y) * 640u * 4u, frame.pixels.data() + static_cast<size_t>(y) * frame.width * 4u,
                        static_cast<size_t>(frame.width) * 4u);
        frame.pixels = std::move(rows);
    }

    // present to the window (Android)
    d.ensureSwapchain();
    if (d.swap.ok)
    {
        uint32_t idx = 0;
        VkResult acq = gs_vk::vk.AcquireNextImageKHR(d.device, d.swap.swapchain, 1000000000ull, d.swap.acquired, VK_NULL_HANDLE, &idx);
        if (acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR)
        {
            d.beginFrameCb();
            VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.image = d.swap.images[idx];
            bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            gs_vk::vk.CmdPipelineBarrier(d.frameCb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(outW), static_cast<int32_t>(outH), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(d.swap.width), static_cast<int32_t>(d.swap.height), 1};
            gs_vk::vk.CmdBlitImage(d.frameCb, d.presentImage, VK_IMAGE_LAYOUT_GENERAL, d.swap.images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            VkImageMemoryBarrier bar2 = bar;
            bar2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            bar2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar2.dstAccessMask = 0;
            gs_vk::vk.CmdPipelineBarrier(d.frameCb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar2);
            d.endFrameCb();
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo psu{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            psu.waitSemaphoreCount = 1;
            psu.pWaitSemaphores = &d.swap.acquired;
            psu.pWaitDstStageMask = &waitStage;
            psu.commandBufferCount = 1;
            psu.pCommandBuffers = &d.frameCb;
            psu.signalSemaphoreCount = 1;
            psu.pSignalSemaphores = &d.swap.rendered;
            gs_vk::vk.ResetFences(d.device, 1, &d.frameFence);
            gs_vk::vk.QueueSubmit(d.queue, 1, &psu, d.frameFence);
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &d.swap.rendered;
            pi.swapchainCount = 1;
            pi.pSwapchains = &d.swap.swapchain;
            pi.pImageIndices = &idx;
            gs_vk::vk.QueuePresentKHR(d.queue, &pi);
            gs_vk::vk.WaitForFences(d.device, 1, &d.frameFence, VK_TRUE, 1000000000ull);
            gs_vk::vk.ResetCommandBuffer(d.frameCb, 0);
            gs_vk::vk.ResetCommandPool(d.device, d.cmdPool, 0);
            d.cbOpen = false;
            d.emptyTrash();
        }
        else
            d.cbOpen = false;
    }
    return frame;
}
