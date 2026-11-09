#pragma once

#include "Core/Export.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

class VulkanContext;

// Shared low-level Vulkan helpers used by Buffer, Swapchain and Renderer.
// Each function throws std::runtime_error on failure so callers stay free of
// VkResult plumbing during engine bring-up.
namespace helpers {

ENGINE_GRAPHICS_API void createBuffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);

ENGINE_GRAPHICS_API void copyBuffer(VulkanContext& context, VkBuffer source, VkBuffer destination, VkDeviceSize size);

// One-off command buffers for transfer operations. The returned buffer must
// be finished with endSingleTimeCommands(), which submits and waits.
ENGINE_GRAPHICS_API VkCommandBuffer beginSingleTimeCommands(VulkanContext& context);
ENGINE_GRAPHICS_API void endSingleTimeCommands(VulkanContext& context, VkCommandBuffer commandBuffer);

ENGINE_GRAPHICS_API void createImage(VulkanContext& context, uint32_t width, uint32_t height, VkFormat format,
                 VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                 VkImage& image, VkDeviceMemory& memory);

ENGINE_GRAPHICS_API VkImageView createImageView(VulkanContext& context, VkImage image, VkFormat format,
                            VkImageAspectFlags aspectFlags);

// Reads an entire binary file (used for SPIR-V shader modules).
ENGINE_GRAPHICS_API std::vector<char> readFile(const std::string& path);

} // namespace helpers
} // namespace engine
