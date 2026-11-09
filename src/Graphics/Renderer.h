#pragma once
#include "Core/Export.h"

#include <vulkan/vulkan.h>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include "Graphics/Buffer.h"
#include "Graphics/HudRenderer.h"
#include "Graphics/RenderQueue.h"

namespace engine {

class VulkanContext;
class Window;
class Swapchain;
class GraphicsPipeline;

// Per-frame data consumed by cube.vert: camera matrices only. Per-entity
// model matrices travel through push constants instead, so one frame can
// draw any number of entities with a single uniform buffer update.
struct UniformBufferObject {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
};

// ---------------------------------------------------------------------------
// Renderer
//
// Owns everything needed to draw frames: per-frame command buffers,
// uniform buffers + descriptor sets, synchronization primitives, and the
// cube's vertex/index buffers. Uses a classic frames-in-flight scheme:
// the configured number of frames are recorded/submitted concurrently,
// each with its own command buffer, uniform buffer and sync objects, so
// the CPU never overwrites data the GPU is still reading. Low-latency
// presentation modes use one frame in flight (see below), trading some
// CPU/GPU overlap for a shorter sample-to-display queue.
// ---------------------------------------------------------------------------
class ENGINE_GRAPHICS_API Renderer {
public:
    static constexpr int kMaxFramesInFlight = 2;

    Renderer(VulkanContext& context, Window& window, Swapchain& swapchain, GraphicsPipeline& pipeline,
             uint32_t maxFramesInFlight = kMaxFramesInFlight);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Draws one frame: camera matrices go to the uniform buffer, each
    // item's model matrix is pushed per draw. Transparently recreates
    // the swapchain when the window has been resized. Returns false when
    // no frame was submitted/presented (e.g. the swapchain was rebuilt),
    // so callers must not count that iteration as a displayed frame.
    bool drawFrame(const glm::mat4& view, const glm::mat4& projection, std::span<const DrawItem> drawItems);

    [[nodiscard]] uint32_t framesInFlight() const { return m_framesInFlight; }

    // Mailbox/Immediate can replace or present work without waiting for
    // a refresh boundary; allowing the CPU to run a second frame ahead
    // would recreate much of FIFO's input queue. Keep those modes to a
    // single in-flight frame, and retain two for FIFO-style throughput.
    [[nodiscard]] static constexpr uint32_t framesInFlightForPresentMode(VkPresentModeKHR mode) {
        return (mode == VK_PRESENT_MODE_MAILBOX_KHR || mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
                   ? 1u
                   : static_cast<uint32_t>(kMaxFramesInFlight);
    }

    // Sets the FPS HUD overlay text (empty disables it). Cheap: the
    // overlay re-lays-out only when the text actually changes.
    void setHudText(std::string text) { m_hud.setText(std::move(text)); }

private:
    void createCommandBuffers();
    void createSyncObjects();
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    void createDescriptorPool();
    void createDescriptorSets();

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                             std::span<const DrawItem> drawItems);
    void updateUniformBuffer(const glm::mat4& view, const glm::mat4& projection);
    void recreateSwapchain();
    void createRenderFinishedSemaphores();

    VulkanContext& m_context;
    Window& m_window;
    Swapchain& m_swapchain;
    GraphicsPipeline& m_pipeline;
    HudRenderer m_hud;

    std::vector<VkCommandBuffer> m_commandBuffers;

    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    uint32_t m_framesInFlight = kMaxFramesInFlight;
    uint32_t m_currentFrame = 0;

    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    uint32_t m_indexCount = 0;

    std::vector<Buffer> m_uniformBuffers;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
};

} // namespace engine
