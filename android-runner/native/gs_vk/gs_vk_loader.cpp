#include "gs_vk_loader.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gs_vk
{
    Functions vk;

    namespace
    {
        void *s_library = nullptr;
        PFN_vkGetInstanceProcAddr s_getInstanceProcAddr = nullptr;

#if defined(_WIN32)
        void *OpenLibrary(const char *path) { return reinterpret_cast<void *>(LoadLibraryA(path)); }
        void *Symbol(void *lib, const char *name) { return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(lib), name)); }
        void CloseLibrary(void *lib) { FreeLibrary(static_cast<HMODULE>(lib)); }
#else
        void *OpenLibrary(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
        void *Symbol(void *lib, const char *name) { return dlsym(lib, name); }
        void CloseLibrary(void *lib) { dlclose(lib); }
#endif
    }

    bool Load()
    {
        if (s_library)
            return true;
#if defined(__ANDROID__)
        const char *candidates[] = {"libvulkan.so"};
#else
        // Desktop probe: SwiftShader's ICD DLL exports the loader entry points.
        // PS2X_VK_ICD overrides; then a few known locations.
        static char icd[512];
        const char *candidates[4] = {};
        if (const char *env = std::getenv("PS2X_VK_ICD"); env && *env)
        {
            std::snprintf(icd, sizeof(icd), "%s", env);
            candidates[0] = icd;
        }
        else
        {
            candidates[0] = "C:\\Program Files\\Unity Hub\\vk_swiftshader.dll";
            candidates[1] = "vk_swiftshader.dll";
        }
#endif
        for (const char *path : candidates)
        {
            if (!path)
                continue;
            s_library = OpenLibrary(path);
            if (!s_library)
            {
#if defined(_WIN32)
                std::fprintf(stderr, "[gs-vk] LoadLibrary('%s') failed (win32 error %lu)\n", path, GetLastError());
#endif
                continue;
            }
            auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(Symbol(s_library, "vkGetInstanceProcAddr"));
            if (!getInstanceProcAddr)
            {
                std::fprintf(stderr, "[gs-vk] %s: vkGetInstanceProcAddr not exported\n", path);
                CloseLibrary(s_library);
                s_library = nullptr;
                continue;
            }
            std::memset(&vk, 0, sizeof(vk));
            s_getInstanceProcAddr = getInstanceProcAddr;
#define GSVK_LOAD(name) vk.name = reinterpret_cast<PFN_vk##name>(getInstanceProcAddr(VK_NULL_HANDLE, "vk" #name))
            GSVK_LOAD(CreateInstance);
            GSVK_LOAD(EnumerateInstanceLayerProperties);
            GSVK_LOAD(EnumerateInstanceExtensionProperties);
#undef GSVK_LOAD
            if (!vk.CreateInstance)
            {
                std::fprintf(stderr, "[gs-vk] %s: vkCreateInstance missing\n", path);
                CloseLibrary(s_library);
                s_library = nullptr;
                continue;
            }
            return true; // everything else resolves after vkCreateInstance (ResolveInstanceFunctions)
        }
        return false;
    }

    bool ResolveInstanceFunctions(VkInstance instance)
    {
        if (!s_getInstanceProcAddr)
            return false;
        auto get = [&](const char *name) { return s_getInstanceProcAddr(instance, name); };
#define GSVK_LOAD(name) vk.name = reinterpret_cast<PFN_vk##name>(get("vk" #name))
        GSVK_LOAD(EnumeratePhysicalDevices);
        GSVK_LOAD(DestroyInstance);
        GSVK_LOAD(GetPhysicalDeviceProperties);
        GSVK_LOAD(GetPhysicalDeviceMemoryProperties);
        GSVK_LOAD(GetPhysicalDeviceQueueFamilyProperties);
        GSVK_LOAD(GetPhysicalDeviceFeatures);
        GSVK_LOAD(GetPhysicalDeviceFormatProperties);
        GSVK_LOAD(GetPhysicalDeviceSurfaceSupportKHR);
        GSVK_LOAD(GetPhysicalDeviceSurfaceCapabilitiesKHR);
        GSVK_LOAD(GetPhysicalDeviceSurfaceFormatsKHR);
        GSVK_LOAD(CreateDevice);
        GSVK_LOAD(GetDeviceProcAddr);
        GSVK_LOAD(DestroySurfaceKHR);
        GSVK_LOAD(CreateAndroidSurfaceKHR);
#undef GSVK_LOAD
        return vk.EnumeratePhysicalDevices && vk.CreateDevice && vk.GetPhysicalDeviceProperties;
    }


    bool ResolveDeviceFunctions(VkDevice device)
    {
        if (!vk.GetDeviceProcAddr)
            return false;
        bool ok = true;
#define GSVK_LOAD(name)                              \
    do                                               \
    {                                                \
        vk.name = reinterpret_cast<PFN_vk##name>(vk.GetDeviceProcAddr(device, "vk" #name)); \
        if (!vk.name) { ok = false; std::fprintf(stderr, "[gs-vk] missing device entry point: vk%s\n", #name); } \
    } while (0)
        GSVK_LOAD(DestroyDevice);
        GSVK_LOAD(GetDeviceQueue);
        GSVK_LOAD(CreateCommandPool);
        GSVK_LOAD(DestroyCommandPool);
        GSVK_LOAD(AllocateCommandBuffers);
        GSVK_LOAD(FreeCommandBuffers);
        GSVK_LOAD(ResetCommandPool);
        GSVK_LOAD(BeginCommandBuffer);
        GSVK_LOAD(EndCommandBuffer);
        GSVK_LOAD(ResetCommandBuffer);
        GSVK_LOAD(CmdBeginRenderPass);
        GSVK_LOAD(CmdEndRenderPass);
        GSVK_LOAD(CmdBindPipeline);
        GSVK_LOAD(CmdBindDescriptorSets);
        GSVK_LOAD(CmdBindVertexBuffers);
        GSVK_LOAD(CmdDraw);
        GSVK_LOAD(CmdSetViewport);
        GSVK_LOAD(CmdSetScissor);
        GSVK_LOAD(CmdSetBlendConstants);
        GSVK_LOAD(CmdPushConstants);
        GSVK_LOAD(CreateBuffer);
        GSVK_LOAD(DestroyBuffer);
        GSVK_LOAD(AllocateMemory);
        GSVK_LOAD(FreeMemory);
        GSVK_LOAD(GetBufferMemoryRequirements);
        GSVK_LOAD(GetImageMemoryRequirements);
        vk.CreateSemaphoreF = reinterpret_cast<PFN_vkCreateSemaphore>(vk.GetDeviceProcAddr(device, "vkCreateSemaphore"));
        if (!vk.CreateSemaphoreF) ok = false;
        vk.DestroySemaphoreF = reinterpret_cast<PFN_vkDestroySemaphore>(vk.GetDeviceProcAddr(device, "vkDestroySemaphore"));
        if (!vk.DestroySemaphoreF) ok = false;
        GSVK_LOAD(BindBufferMemory);
        GSVK_LOAD(BindImageMemory);
        GSVK_LOAD(MapMemory);
        GSVK_LOAD(UnmapMemory);
        GSVK_LOAD(FlushMappedMemoryRanges);
        GSVK_LOAD(CreateImage);
        GSVK_LOAD(DestroyImage);
        GSVK_LOAD(CreateImageView);
        GSVK_LOAD(DestroyImageView);
        GSVK_LOAD(CreateSampler);
        GSVK_LOAD(DestroySampler);
        GSVK_LOAD(CreateFramebuffer);
        GSVK_LOAD(DestroyFramebuffer);
        GSVK_LOAD(CreateRenderPass);
        GSVK_LOAD(DestroyRenderPass);
        GSVK_LOAD(CreateShaderModule);
        GSVK_LOAD(DestroyShaderModule);
        GSVK_LOAD(CreatePipelineLayout);
        GSVK_LOAD(DestroyPipelineLayout);
        GSVK_LOAD(CreateGraphicsPipelines);
        GSVK_LOAD(DestroyPipeline);
        GSVK_LOAD(CreateDescriptorSetLayout);
        GSVK_LOAD(DestroyDescriptorSetLayout);
        GSVK_LOAD(AllocateDescriptorSets);
        GSVK_LOAD(FreeDescriptorSets);
        GSVK_LOAD(UpdateDescriptorSets);
        GSVK_LOAD(CreateDescriptorPool);
        GSVK_LOAD(DestroyDescriptorPool);
        GSVK_LOAD(CmdPipelineBarrier);
        GSVK_LOAD(CmdCopyBuffer);
        GSVK_LOAD(CmdCopyBufferToImage);
        GSVK_LOAD(CmdCopyImageToBuffer);
        GSVK_LOAD(CmdBlitImage);
        GSVK_LOAD(CmdClearAttachments);
        GSVK_LOAD(CmdClearColorImage);
        GSVK_LOAD(QueueSubmit);
        GSVK_LOAD(QueuePresentKHR);
        GSVK_LOAD(QueueWaitIdle);
        GSVK_LOAD(DeviceWaitIdle);
        GSVK_LOAD(WaitForFences);
        GSVK_LOAD(ResetFences);
        GSVK_LOAD(CreateFence);
        GSVK_LOAD(DestroyFence);
        GSVK_LOAD(AcquireNextImageKHR);
        GSVK_LOAD(CreateSwapchainKHR);
        GSVK_LOAD(DestroySwapchainKHR);
        GSVK_LOAD(GetSwapchainImagesKHR);
#undef GSVK_LOAD
        if (!vk.CreateSemaphoreF)
        {
            ok = false;
            std::fprintf(stderr, "[gs-vk] missing: vkCreateSemaphore\n");
        }
        if (!vk.DestroySemaphoreF)
        {
            ok = false;
            std::fprintf(stderr, "[gs-vk] missing: vkDestroySemaphore\n");
        }
        return ok;
    }

    void Unload()
    {
        if (s_library)
        {
            CloseLibrary(s_library);
            s_library = nullptr;
        }
    }
}
