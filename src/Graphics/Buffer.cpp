#include "Graphics/Buffer.h"

#include "Graphics/VulkanContext.h"
#include "Graphics/VulkanHelpers.h"

#include <cstring>
#include <utility>

namespace engine {

Buffer::Buffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags properties)
    : m_context(&context), m_size(size) {
    helpers::createBuffer(context, size, usage, properties, m_buffer, m_memory);
}

Buffer::~Buffer() {
    destroy();
}

Buffer::Buffer(Buffer&& other) noexcept
    : m_context(other.m_context),
      m_buffer(std::exchange(other.m_buffer, VK_NULL_HANDLE)),
      m_memory(std::exchange(other.m_memory, VK_NULL_HANDLE)),
      m_size(std::exchange(other.m_size, 0)),
      m_mapped(std::exchange(other.m_mapped, nullptr)) {
    other.m_context = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        m_context = std::exchange(other.m_context, nullptr);
        m_buffer = std::exchange(other.m_buffer, VK_NULL_HANDLE);
        m_memory = std::exchange(other.m_memory, VK_NULL_HANDLE);
        m_size = std::exchange(other.m_size, 0);
        m_mapped = std::exchange(other.m_mapped, nullptr);
    }
    return *this;
}

void Buffer::map() {
    if (m_mapped == nullptr && m_context != nullptr) {
        vkMapMemory(m_context->device(), m_memory, 0, m_size, 0, &m_mapped);
    }
}

void Buffer::unmap() {
    if (m_mapped != nullptr && m_context != nullptr) {
        vkUnmapMemory(m_context->device(), m_memory);
        m_mapped = nullptr;
    }
}

void Buffer::destroy() {
    if (m_context == nullptr) {
        return;
    }
    unmap();
    if (m_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_context->device(), m_buffer, nullptr);
        m_buffer = VK_NULL_HANDLE;
    }
    if (m_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->device(), m_memory, nullptr);
        m_memory = VK_NULL_HANDLE;
    }
    m_context = nullptr;
}

Buffer createDeviceLocalBuffer(VulkanContext& context, const void* data, VkDeviceSize size,
                               VkBufferUsageFlags usage) {
    Buffer staging(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.map();
    std::memcpy(staging.mappedData(), data, static_cast<size_t>(size));
    staging.unmap();

    Buffer deviceLocal(context, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    helpers::copyBuffer(context, staging.handle(), deviceLocal.handle(), size);
    return deviceLocal;
}

} // namespace engine
