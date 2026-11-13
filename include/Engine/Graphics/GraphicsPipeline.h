#pragma once
#include "Core/Export.h"

#include <vulkan/vulkan.h>

#include "Graphics/PipelineState.h"
#include "Graphics/PipelineWarmup.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

namespace engine {

class JobSystem;
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
class ENGINE_GRAPHICS_API GraphicsPipeline {
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

    // --- Async startup warmup --------------------------------------
    // Queues a material's pipeline for frame-budgeted creation
    // instead of compiling it on the spot (see PipelineWarmup.h).
    // Returns false when the material already owns a pipeline or is
    // already queued. Until the queued creation lands, pipelineFor()
    // returns VK_NULL_HANDLE and the renderer keeps the default
    // pipeline bound for that material's draws.
    bool queuePipelineWarmup(uint32_t materialId, const PipelineStateDesc& state,
                             const std::string& vertexShaderFile, const std::string& fragmentShaderFile);
    // Creates up to `maxCreations` DISTINCT pipelines from the
    // warmup queue (materials sharing an already-created state bind
    // for free and do not consume the budget). Call once per frame
    // from the main thread until pipelineWarmupComplete(). Returns
    // how many distinct pipelines this call created.
    size_t processPipelineWarmup(size_t maxCreations, const Swapchain& swapchain);
    [[nodiscard]] size_t pendingPipelineWarmupCount() const { return m_warmup.pendingCount(); }
    [[nodiscard]] bool pipelineWarmupComplete() const { return m_warmup.complete(); }

    // Reads one SPIR-V file into the shader bytecode cache on a
    // JobSystem worker, so pipeline creation never blocks the main
    // thread on disk I/O. Fire-and-forget and idempotent: a file
    // already cached (or already in flight) is not re-read, and a
    // failed read simply leaves the cache empty — the main-thread
    // creation path then reads (and reports) the file itself.
    void preloadShaderAsync(JobSystem& jobs, const std::string& shaderFile);
    [[nodiscard]] size_t preloadedShaderCount() const;

    // Creates the depth-only shadow-map pipeline (shadow.vert, no
    // fragment stage, front-face culling + depth bias) for the given
    // shadow render pass, sharing this manager's pipeline layout. Owned
    // here and destroyed with the other pipelines.
    VkPipeline createShadowPipeline(VkRenderPass shadowRenderPass);
    [[nodiscard]] VkPipeline shadowPipeline() const { return m_shadowPipeline; }

    // Phase 22: scene geometry renders into the renderer's HDR
    // offscreen pass instead of the swapchain pass. Retargets every
    // pipeline (the render-pass attachment formats differ, so old
    // pipelines are destroyed and recreated). Call before any
    // material pipelines are created.
    void setSceneRenderPass(VkRenderPass sceneRenderPass);

    // Creates the skybox pipeline (inside-out cube, depth-write off,
    // samples the environment cubemap through the shared layout).
    VkPipeline createSkyboxPipeline(VkRenderPass sceneRenderPass);
    [[nodiscard]] VkPipeline skyboxPipeline() const { return m_skyboxPipeline; }

    [[nodiscard]] VkPipeline handle() const { return m_pipeline; }
    [[nodiscard]] VkPipelineLayout layout() const { return m_layout; }
    [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const { return m_descriptorSetLayout; }
    [[nodiscard]] VkPipelineCache pipelineCache() const { return m_pipelineCache; }
    [[nodiscard]] size_t cachedPipelineCount() const { return m_distinctPipelines; }

private:
    void createDescriptorSetLayout();
    void createPipelineCache();
    VkPipeline createPipeline(VkRenderPass renderPass, const PipelineStateDesc& state,
                              const std::string& vertexShaderFile, const std::string& fragmentShaderFile);
    [[nodiscard]] VkShaderModule createShaderModule(const char* fileName) const;

    // SPIR-V bytes read by preloadShaderAsync workers. Shared so a
    // job that outlives this manager still lands safely (its bytes
    // are simply never consumed).
    struct ShaderBytecodeCache;
    std::shared_ptr<ShaderBytecodeCache> m_shaderCache;

    VulkanContext& m_context;
    PipelineWarmupQueue m_warmup;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_skyboxPipeline = VK_NULL_HANDLE;
    VkRenderPass m_sceneRenderPass = VK_NULL_HANDLE;
    PipelineStateCache m_stateCache;
    std::unordered_map<uint32_t, VkPipeline> m_materialPipelines;
    std::unordered_map<uint64_t, VkPipeline> m_pipelinesByState; // (state, shaders) -> pipeline
    size_t m_distinctPipelines = 0;
};

} // namespace engine
