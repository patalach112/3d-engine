#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

#include "Graphics/Buffer.h"
#include "Graphics/HudFont.h"

namespace engine {

class VulkanContext;
class Swapchain;

// ---------------------------------------------------------------------------
// HudRenderer
//
// Zero-overhead screen-space text overlay: a dedicated HUD pipeline (no
// descriptors, no depth test, alpha blending) draws pre-built bitmap-font
// triangles (HudFont) from a persistently mapped dynamic vertex buffer.
// Text is re-laid-out only when it changes (the Application refreshes it
// a few times per second) or the viewport size changes; recording a frame
// costs one pipeline bind + one draw call. Must be recorded inside the
// active render pass, after the scene draws.
// ---------------------------------------------------------------------------
class HudRenderer {
public:
    HudRenderer(VulkanContext& context, const Swapchain& swapchain);
    ~HudRenderer();

    HudRenderer(const HudRenderer&) = delete;
    HudRenderer& operator=(const HudRenderer&) = delete;
    HudRenderer(HudRenderer&&) = delete;
    HudRenderer& operator=(HudRenderer&&) = delete;

    void setText(std::string text);
    // Records the overlay into an open render pass. No-op without text.
    void record(VkCommandBuffer commandBuffer, VkExtent2D extent);

    [[nodiscard]] bool hasText() const { return !m_text.empty(); }
    [[nodiscard]] uint32_t vertexCount() const { return m_vertexCount; }

private:
    void createPipeline(const Swapchain& swapchain);
    void createVertexBuffer();
    [[nodiscard]] VkShaderModule createShaderModule(const char* fileName) const;
    void rebuildVertices(VkExtent2D extent);

    VulkanContext& m_context;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    Buffer m_vertexBuffer;
    std::string m_text;
    uint32_t m_vertexCount = 0;
    VkExtent2D m_builtExtent{0, 0};
    bool m_dirty = true;
};

} // namespace engine
