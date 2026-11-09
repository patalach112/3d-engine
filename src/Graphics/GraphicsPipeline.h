#pragma once

#include <vulkan/vulkan.h>

#include "Graphics/PipelineState.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace engine {

class VulkanContext;
class Swapchain;

// ---------------------------------------------------------------------------
// GraphicsPipeline — pipeline state object (PSO) manager.
//
// Owns the shared descriptor set layout + pipeline layout, a
// VkPipelineCache, and every graphics pipeline created through it. The
// default pipeline (Phase 1 cube state) is created up front; additional
// pipelines are created deterministically per material on demand:
// materials are associated with stable ids (AssetManager), pipeline
// states are deduplicated by their PipelineStateDesc hash, so N
// materials sharing one state cost exactly one VkPipeline. All pipelines
// share the one layout, so descriptor set bindings stay valid across
// per-material pipeline switches in the render loop.
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

    // Returns the pipeline previously created for a material, or
    // VK_NULL_HANDLE when none exists (caller keeps the current binding).
    [[nodiscard]] VkPipeline pipelineFor(uint32_t materialId) const;

    // Creates (or returns the existing) pipeline for a material. States
    // are deduplicated: a state seen before reuses its VkPipeline.
    VkPipeline getOrCreatePipeline(uint32_t materialId, const PipelineStateDesc& state,
                                   const std::string& vertexShaderFile, const std::string& fragmentShaderFile,
                                   const Swapchain& swapchain);

    [[nodiscard]] VkPipeline handle() const { return m_pipeline; }
    [[nodiscard]] VkPipelineLayout layout() const { return m_layout; }
    [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const { return m_descriptorSetLayout; }
    [[nodiscard]] VkPipelineCache pipelineCache() const { return m_pipelineCache; }
    [[nodiscard]] size_t cachedPipelineCount() const { return m_distinctPipelines; }

private:
    void createDescriptorSetLayout();
    void createPipelineCache();
    VkPipeline createPipeline(const Swapchain& swapchain, const PipelineStateDesc& state,
                              const std::string& vertexShaderFile, const std::string& fragmentShaderFile);
    [[nodiscard]] VkShaderModule createShaderModule(const char* fileName) const;

    VulkanContext& m_context;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    PipelineStateCache m_stateCache;
    std::unordered_map<uint32_t, VkPipeline> m_materialPipelines;
    std::unordered_map<uint64_t, VkPipeline> m_pipelinesByState; // (state, shaders) -> pipeline
    size_t m_distinctPipelines = 0;
};

} // namespace engine
