#pragma once
#include "Core/Export.h"

#include <vulkan/vulkan.h>

#include <glm/mat4x4.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "Graphics/Buffer.h"
#include "Graphics/HudRenderer.h"
#include "Graphics/RenderQueue.h"
#include "Graphics/UIRenderer.h"
#include "Graphics/ShadowMap.h"

namespace engine {

class VulkanContext;
class Window;
class Swapchain;
class GraphicsPipeline;

// Per-frame data consumed by the cube shaders: camera matrices plus the
// directional light's view-projection (shadow reprojection) and
// direction. Per-entity model matrices travel through push constants
// instead, so one frame can draw any number of entities with a single
// uniform buffer update.
inline constexpr uint32_t kMaxFrameLights = 16;

// One frame of lighting for the renderer (Phase 21): cascaded shadow
// matrices + packed dynamic lights, gathered by the scene-side
// LightingSystem. A default-constructed value reproduces the legacy
// single-cascade behaviour.
struct FrameLighting {
    uint32_t cascadeCount = 1;
    bool cascadesFilled = false;
    glm::vec4 cascadeSplits{60.0f, 60.0f, 60.0f, 60.0f};
    std::array<glm::mat4, kMaxShadowCascades> cascades{};
    glm::vec4 lightCounts{0.0f}; // x point count, y spot count, z total
    std::array<glm::vec4, kMaxFrameLights> lightPositionRange{};
    std::array<glm::vec4, kMaxFrameLights> lightColorIntensity{};
    std::array<glm::vec4, kMaxFrameLights> lightDirectionType{};
    std::array<glm::vec4, kMaxFrameLights> lightCone{};
    glm::vec3 directionalDirection{0.35f, 0.8f, 0.45f};
    float directionalRadiance = 1.0f;
};

// Cinematic post-processing settings (Phase 22), fed from
// [Render] config by the application. The scene renders HDR into an
// offscreen target; bloom is extracted and blurred, then the
// composite applies exposure + ACES filmic tone mapping + sRGB.
struct PostSettings {
    float exposure = 1.0f;
    bool bloomEnabled = true;
    float bloomStrength = 0.55f;
    float bloomThreshold = 1.0f; // HDR luminance where bloom starts
    bool skyboxEnabled = true;
    float environmentIntensity = 1.0f;
};

// One CPU-side texture for syncMaterialTextures: RGBA8 pixels in
// dense slot order (slot = index in the span).
struct MaterialTextureUpload {
    uint32_t width = 0;
    uint32_t height = 0;
    std::span<const uint8_t> rgba;
};

// Per-frame uniform layout - MUST stay byte-identical with the GLSL
// `Frame` block in cube.vert / cube.frag.
struct UniformBufferObject {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    std::array<glm::mat4, kMaxShadowCascades> lightViewProj;
    glm::vec4 lightDirection{0.0f, 1.0f, 0.0f, 0.0f}; // xyz toward the key light
    glm::vec4 cascadeSplits{60.0f, 60.0f, 60.0f, 60.0f}; // view-depth far plane per cascade
    glm::vec4 lightCounts{0.0f}; // x point count, y spot count, z total
    std::array<glm::vec4, kMaxFrameLights> lightPositionRange{};
    std::array<glm::vec4, kMaxFrameLights> lightColorIntensity{};
    std::array<glm::vec4, kMaxFrameLights> lightDirectionType{};
    std::array<glm::vec4, kMaxFrameLights> lightCone{};
    // x: environment intensity, y: prefilter max LOD, z: skybox on/off
    glm::vec4 environmentParams{1.0f, 4.0f, 1.0f, 0.0f};
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
    bool drawFrame(const glm::mat4& view, const glm::mat4& projection, std::span<const DrawItem> drawItems,
                   const FrameLighting& lighting = {});

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

    // Accumulates this frame's Phase 26 UI primitives (panels,
    // widgets). Issue draw calls between frames; the batch is
    // uploaded and flushed as one overlay draw call inside the
    // present pass, then cleared.
    [[nodiscard]] UIBatch& uiBatch() { return m_ui.batch(); }

    void setPostSettings(const PostSettings& settings) { m_postSettings = settings; }
    [[nodiscard]] const PostSettings& postSettings() const { return m_postSettings; }

    // Mirrors the asset registry's texture table into the GPU
    // material texture array (descriptor binding 2). A no-op when
    // registryVersion matches the last sync.
    void syncMaterialTextures(std::span<const MaterialTextureUpload> textures, uint64_t registryVersion);

private:
    void createCommandBuffers();
    void createSyncObjects();
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    void createDescriptorPool();
    void createDescriptorSets();

    void createShadowResources();
    void recordShadowCascades(VkCommandBuffer commandBuffer, std::span<const DrawItem> drawItems,
                              const std::array<glm::mat4, kMaxShadowCascades>& cascadeMatrices,
                              uint32_t cascadeCount);
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                             std::span<const DrawItem> drawItems,
                             const std::array<glm::mat4, kMaxShadowCascades>& cascadeMatrices,
                             uint32_t cascadeCount);
    void updateUniformBuffer(const glm::mat4& view, const glm::mat4& projection,
                             const FrameLighting& lighting);
    void recreateSwapchain();
    void createRenderFinishedSemaphores();

    // --- Phase 22: HDR scene pass, IBL resources, post chain -------
    void createSceneRenderPass();
    void createHdrTargets();
    void destroyHdrTargets();
    void createEnvironmentResources();
    void createPostResources();
    void destroyPostResources();
    void writePostDescriptorSets();
    void recordBloomChain(VkCommandBuffer commandBuffer);
    void recordComposite(VkCommandBuffer commandBuffer, uint32_t imageIndex);

    VulkanContext& m_context;
    Window& m_window;
    Swapchain& m_swapchain;
    GraphicsPipeline& m_pipeline;
    HudRenderer m_hud;
    UIRenderer m_ui;

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

    // Directional shadow map (depth-only). Fixed size, independent of the
    // swapchain, so resize never recreates it.
    struct ShadowTarget {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    std::array<ShadowTarget, kMaxShadowCascades> m_shadowTargets{};

    PostSettings m_postSettings;

    // Generic owned image (environment maps, LUT, textures, targets).
    struct GpuImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    // HDR offscreen scene rendering (Phase 22): scene pass + one
    // RGBA16F color/depth target per frame in flight.
    static constexpr uint32_t kBloomLevels = 5;
    static constexpr uint32_t kPrefilterLevels = 5;
    VkRenderPass m_sceneRenderPass = VK_NULL_HANDLE;
    struct HdrTarget {
        GpuImage color;
        GpuImage depth;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    std::vector<HdrTarget> m_hdrTargets;

    // Image-based lighting resources (generated once from the
    // procedural sky; see Graphics/EnvironmentMap).
    GpuImage m_environmentCube;
    GpuImage m_irradianceCube;
    GpuImage m_prefilterCube;
    GpuImage m_brdfLut;
    GpuImage m_whiteTexture; // 1x1 fallback for untextured array slots
    VkSampler m_environmentSampler = VK_NULL_HANDLE;
    VkSampler m_materialSampler = VK_NULL_HANDLE;
    std::vector<GpuImage> m_materialTextures;
    uint64_t m_syncedTextureVersion = ~uint64_t{0};

    // Post-processing: bloom mip chain + composite (fullscreen).
    VkRenderPass m_bloomRenderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_postDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_postDescriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_postPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_bloomDownPipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomUpPipeline = VK_NULL_HANDLE;
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;
    struct BloomLevel {
        GpuImage target;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkExtent2D extent{1, 1};
    };
    struct BloomChain {
        std::array<BloomLevel, kBloomLevels> levels;
        std::array<VkDescriptorSet, kBloomLevels> downSets{};
        std::array<VkDescriptorSet, kBloomLevels> upSets{};
        VkDescriptorSet compositeSet = VK_NULL_HANDLE;
    };
    std::vector<BloomChain> m_bloomChains;
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkRenderPass m_shadowRenderPass = VK_NULL_HANDLE;
};

} // namespace engine
