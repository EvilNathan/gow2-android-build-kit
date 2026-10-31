// Minimal Vulkan loader for the GS backend — dlopen()ed, no link dependency
// (the kit's CMakeLists relies on that: no API level requirement).
//
// Android: the system "libvulkan.so" loader. Desktop (offscreen probe): the
// SwiftShader ICD DLL directly (it exports the vk* entry points itself); the
// path comes from PS2X_VK_ICD or the default Unity Hub location.
#pragma once

#include <cstdint>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#ifdef VK_USE_PLATFORM_ANDROID_KHR
// the Android surface type comes from vulkan_android.h when the platform macro is set
#endif

namespace gs_vk
{
    // Loads the Vulkan library and the core + Android-surface entry points.
    // Returns false when the library is missing or the entry points are absent.
    bool Load();
    void Unload();

    // Global function table filled by Load(). Functions used with a device are
    // resolved per device through ResolveDeviceFunctions().
    struct Functions
    {
        // instance level
        PFN_vkCreateInstance CreateInstance = nullptr;
        PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
        PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
        PFN_vkDestroyInstance DestroyInstance = nullptr;
        PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
        PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
        PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
        PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
        PFN_vkCreateDevice CreateDevice = nullptr;
        PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
        PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
        PFN_vkCreateAndroidSurfaceKHR CreateAndroidSurfaceKHR = nullptr;
        // device level (instance-generic pointers; use vkGetDeviceProcAddr results for hot paths)
        PFN_vkDestroyDevice DestroyDevice = nullptr;
        PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
        PFN_vkCreateCommandPool CreateCommandPool = nullptr;
        PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
        PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
        PFN_vkResetCommandPool ResetCommandPool = nullptr;
        PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
        PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
        PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
        PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
        PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
        PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
        PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
        PFN_vkCmdDraw CmdDraw = nullptr;
        PFN_vkCmdSetViewport CmdSetViewport = nullptr;
        PFN_vkCmdSetScissor CmdSetScissor = nullptr;
        PFN_vkCmdSetBlendConstants CmdSetBlendConstants = nullptr;
        PFN_vkCmdPushConstants CmdPushConstants = nullptr;
        PFN_vkCreateBuffer CreateBuffer = nullptr;
        PFN_vkDestroyBuffer DestroyBuffer = nullptr;
        PFN_vkAllocateMemory AllocateMemory = nullptr;
        PFN_vkFreeMemory FreeMemory = nullptr;
        PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
        PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
        PFN_vkCreateSemaphore CreateSemaphoreF = nullptr;
        PFN_vkDestroySemaphore DestroySemaphoreF = nullptr;
        PFN_vkBindBufferMemory BindBufferMemory = nullptr;
        PFN_vkBindImageMemory BindImageMemory = nullptr;
        PFN_vkMapMemory MapMemory = nullptr;
        PFN_vkUnmapMemory UnmapMemory = nullptr;
        PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges = nullptr;
        PFN_vkCreateImage CreateImage = nullptr;
        PFN_vkDestroyImage DestroyImage = nullptr;
        PFN_vkCreateImageView CreateImageView = nullptr;
        PFN_vkDestroyImageView DestroyImageView = nullptr;
        PFN_vkCreateSampler CreateSampler = nullptr;
        PFN_vkDestroySampler DestroySampler = nullptr;
        PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
        PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
        PFN_vkCreateRenderPass CreateRenderPass = nullptr;
        PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
        PFN_vkCreateShaderModule CreateShaderModule = nullptr;
        PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
        PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
        PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
        PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
        PFN_vkDestroyPipeline DestroyPipeline = nullptr;
        PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
        PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
        PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
        PFN_vkFreeDescriptorSets FreeDescriptorSets = nullptr;
        PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
        PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
        PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
        PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
        PFN_vkCmdCopyBuffer CmdCopyBuffer = nullptr;
        PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
        PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
        PFN_vkCmdBlitImage CmdBlitImage = nullptr;
        PFN_vkCmdClearAttachments CmdClearAttachments = nullptr;
        PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
        PFN_vkQueueSubmit QueueSubmit = nullptr;
        PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
        PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
        PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
        PFN_vkWaitForFences WaitForFences = nullptr;
        PFN_vkResetFences ResetFences = nullptr;
        PFN_vkCreateFence CreateFence = nullptr;
        PFN_vkDestroyFence DestroyFence = nullptr;
        PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
        PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
        PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
        PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    };
    extern Functions vk;

    // Instance level functions that need a live instance (enumeration, device
    // creation). Call once after a successful vkCreateInstance.
    bool ResolveInstanceFunctions(VkInstance instance);

    // Resolves the device level functions through vkGetDeviceProcAddr. Returns
    // false when a required entry point is missing.
    bool ResolveDeviceFunctions(VkDevice device);
}
