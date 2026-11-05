// runtime/render/vulkan — the Vulkan IRenderDevice (Quest port, phase 2): the Metal backend's structure (passes recorded
// per target, encoded together in one render pass; fixed depth passes; composite; resolve to an external image) on
// Vulkan 1.1 + VK_KHR_create_renderpass2, with GLSL generated per combiner (glsl_emitter) and compiled at run time by
// shaderc. Runs headless (lavapipe for tests) or on a device the caller owns (OpenXR, phase 4).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>
#include "render_device.h"
namespace ar {
struct VulkanDeviceOptions {
    bool validation = false;              // VK_LAYER_KHRONOS_validation + a messenger counting its errors (headless only)
    std::string pipeline_cache_path;      // VkPipelineCache loaded from / saved to this file ("" = none)
    std::string prefer_device;            // a substring of the physical device's name to pick ("" = the first)
};
// The handles of a device the caller created (OpenXR's xrCreateVulkanDeviceKHR); the render device never destroys them.
struct VulkanExternalDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t api_version = VK_API_VERSION_1_1;
    bool extended_dynamic_state = false;  // VK_EXT_extended_dynamic_state was enabled on the device
};
// What ExternalView::color / depth and encode_composite's external_texture point to under Vulkan: an image the caller
// owns (an OpenXR swapchain image), in `layout` before the call and left in it after.
struct VulkanExternalImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;    // a 2D view of the slice to draw (ExternalView::slice is ignored)
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
};
struct VulkanDeviceInfo {
    std::string name, driver;
    uint32_t api_version = 0;
    bool extended_dynamic_state = false, depth_resolve_max = false, lazily_allocated = false;
    uint32_t max_samples = 1;
    uint64_t validation_errors = 0, validation_warnings = 0;
    uint64_t pipelines_built = 0, shader_modules = 0, draws_last_frame = 0, render_passes_last_frame = 0;
    double compile_ms_total = 0.0;
};
std::unique_ptr<IRenderDevice> create_vulkan_device(const VulkanDeviceOptions &opt, std::string *error);
std::unique_ptr<IRenderDevice> create_vulkan_device_on(const VulkanExternalDevice &ext, const VulkanDeviceOptions &opt,
                                                       std::string *error);
// dev must come from create_vulkan_device(_on).
VulkanDeviceInfo vulkan_device_info(const IRenderDevice &dev);
bool vulkan_device_save_pipeline_cache(IRenderDevice &dev);
}
