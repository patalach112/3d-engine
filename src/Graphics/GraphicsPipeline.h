#pragma once

#include <vulkan/vulkan.h>

namespace engine {

class VulkanContext;
class Swapchain;

// ---------------------------------------------------------------------------
// GraphicsPipeline
//
// The single fixed-function + programmable pipeline state for the Phase 1
// cube: vertex input (position/color), one uniform-buffer descriptor set
// (the MVP matrices), depth testing, back-face culling, and a dynamic
// viewport/scissor so the pipeline never needs recreation on resize.
// ---------------------------------------------------------------------------
class GraphicsPipeline {
public:
    GraphicsPipeline(VulkanContext& context, const Swapchain& swapchain);
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(GraphicsPipeline&&) = delete;
    GraphicsPipeline& operator=(GraphicsPipeline&&) = delete;

    void bind(VkCommandBuffer commandBuffer) const;

    [[nodiscard]] VkPipeline handle() const { return m_pipeline; }
    [[nodiscard]] VkPipelineLayout layout() const { return m_layout; }
    [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const { return m_descriptorSetLayout; }

private:
    void createDescriptorSetLayout();
    void createPipeline(const Swapchain& swapchain);
    [[nodiscard]] VkShaderModule createShaderModule(const char* fileName) const;

    VulkanContext& m_context;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace engine
