#pragma once

#include <vulkan/vulkan.h>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <vector>

#include "Graphics/Buffer.h"

namespace engine {

class VulkanContext;
class Window;
class Swapchain;
class GraphicsPipeline;

// Per-frame data consumed by cube.vert.
struct UniformBufferObject {
    glm::mat4 model{1.0f};
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
};

// ---------------------------------------------------------------------------
// Renderer
//
// Owns everything needed to draw frames: per-frame command buffers,
// uniform buffers + descriptor sets, synchronization primitives, and the
// cube's vertex/index buffers. Uses a classic frames-in-flight scheme:
// at most kMaxFramesInFlight frames are recorded/submitted concurrently,
// each with its own command buffer, uniform buffer and sync objects, so
// the CPU never overwrites data the GPU is still reading.
// ---------------------------------------------------------------------------
class Renderer {
public:
    static constexpr int kMaxFramesInFlight = 2;

    Renderer(VulkanContext& context, Window& window, Swapchain& swapchain, GraphicsPipeline& pipeline);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Draws one frame with the given transforms. Transparently recreates
    // the swapchain when the window has been resized.
    void drawFrame(const UniformBufferObject& ubo);

private:
    void createCommandBuffers();
    void createSyncObjects();
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    void createDescriptorPool();
    void createDescriptorSets();

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void updateUniformBuffer(const UniformBufferObject& ubo);
    void recreateSwapchain();
    void createRenderFinishedSemaphores();

    VulkanContext& m_context;
    Window& m_window;
    Swapchain& m_swapchain;
    GraphicsPipeline& m_pipeline;

    std::vector<VkCommandBuffer> m_commandBuffers;

    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    uint32_t m_currentFrame = 0;

    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    uint32_t m_indexCount = 0;

    std::vector<Buffer> m_uniformBuffers;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
};

} // namespace engine
