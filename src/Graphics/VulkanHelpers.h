#pragma once

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

void createBuffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);

void copyBuffer(VulkanContext& context, VkBuffer source, VkBuffer destination, VkDeviceSize size);

// One-off command buffers for transfer operations. The returned buffer must
// be finished with endSingleTimeCommands(), which submits and waits.
VkCommandBuffer beginSingleTimeCommands(VulkanContext& context);
void endSingleTimeCommands(VulkanContext& context, VkCommandBuffer commandBuffer);

void createImage(VulkanContext& context, uint32_t width, uint32_t height, VkFormat format,
                 VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                 VkImage& image, VkDeviceMemory& memory);

VkImageView createImageView(VulkanContext& context, VkImage image, VkFormat format,
                            VkImageAspectFlags aspectFlags);

// Reads an entire binary file (used for SPIR-V shader modules).
std::vector<char> readFile(const std::string& path);

} // namespace helpers
} // namespace engine
