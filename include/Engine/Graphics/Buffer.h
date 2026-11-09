#pragma once
#include "Core/Export.h"

#include <vulkan/vulkan.h>

#include <cstddef>

namespace engine {

class VulkanContext;

// ---------------------------------------------------------------------------
// Buffer
//
// RAII owner of a VkBuffer plus its VkDeviceMemory. Move-only. Optionally
// keeps the memory persistently mapped (used by the per-frame uniform
// buffers, which the CPU rewrites every frame).
// ---------------------------------------------------------------------------
class ENGINE_GRAPHICS_API Buffer {
public:
    Buffer() = default;
    Buffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags properties);
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    [[nodiscard]] VkBuffer handle() const { return m_buffer; }
    [[nodiscard]] VkDeviceSize size() const { return m_size; }
    [[nodiscard]] void* mappedData() const { return m_mapped; }

    void map();
    void unmap();

private:
    void destroy();

    VulkanContext* m_context = nullptr;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkDeviceSize m_size = 0;
    void* m_mapped = nullptr;
};

// Creates a device-local buffer and uploads `data` into it through a
// staging buffer. Ideal for static geometry: fast GPU-side reads, written
// once by the CPU. `usage` must not include transfer bits; they are added.
ENGINE_GRAPHICS_API Buffer createDeviceLocalBuffer(VulkanContext& context, const void* data, VkDeviceSize size,
                               VkBufferUsageFlags usage);

} // namespace engine
