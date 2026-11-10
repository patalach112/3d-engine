#pragma once
#include "Core/Export.h"

#include <vulkan/vulkan.h>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "Graphics/Buffer.h"

namespace engine {

class VulkanContext;
class Swapchain;

// ---------------------------------------------------------------------------
// UIVertex / UIBatch / UIRenderer (Phase 26)
//
// Immediate-mode screen-space UI drawn by one analytic-shader
// pipeline (shaders/ui.vert + ui.frag): every primitive is a single
// quad whose fragment shader evaluates a rounded-box SDF, so
// rounded rectangles, borders and solid colors render pixel-perfect
// at any size with no texture atlas and no descriptors. UIBatch is
// the pure-CPU accumulator (headless-testable); UIRenderer uploads
// the per-frame batch into persistently mapped dynamic buffers and
// flushes it as ONE indexed draw call inside the present render
// pass, after the 3D scene pass.
//
// Coordinates are pixels with a top-left origin (y down), matching
// the HUD conventions. uv spans 0..1 across each quad; the fragment
// shader recovers the rect's exact pixel size from the uv
// derivatives, which keeps radius/border widths in true pixel
// units without extra attributes.
// ---------------------------------------------------------------------------

struct UIVertex {
    glm::vec2 position{0.0f, 0.0f}; // pixels, top-left origin
    glm::vec2 uv{0.0f, 0.0f};       // 0..1 across the quad
    glm::vec4 color{1.0f};          // fill RGBA
    float cornerRadius = 0.0f;      // pixels (clamped to half the short side)
    float borderWidth = 0.0f;       // pixels, 0 = no border
    glm::vec4 borderColor{0.0f};    // RGBA, used when borderWidth > 0
};

struct UIRectStyle {
    float cornerRadius = 0.0f;   // pixels
    float borderWidth = 0.0f;    // pixels, 0 = no border
    glm::vec4 borderColor{0.0f}; // RGBA, used when borderWidth > 0
};

// Union of all queued primitive rectangles, in pixels.
struct UIBounds {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool empty = true;

    [[nodiscard]] float width() const { return empty ? 0.0f : maxX - minX; }
    [[nodiscard]] float height() const { return empty ? 0.0f : maxY - minY; }
};

// Per-frame CPU geometry accumulator. begin() starts a fresh batch;
// draw calls append quads (4 vertices + 6 indices each); the batch
// is capacity-capped and truncates whole quads rather than ever
// overflowing (truncated() reports it). drawText is a stub for the
// future SDF/bitmap glyph batcher: it records the item but emits no
// geometry yet.
class ENGINE_GRAPHICS_API UIBatch {
public:
    static constexpr uint32_t kMaxVertices = 65536;
    static constexpr uint32_t kMaxIndices = 98304; // 1.5x quads' worth

    void begin(uint32_t viewportWidth, uint32_t viewportHeight);
    void clear();

    void drawRect(float x, float y, float width, float height, const glm::vec4& color);
    void drawRoundedRect(float x, float y, float width, float height, float cornerRadius,
                         const glm::vec4& color);
    void drawRoundedRect(float x, float y, float width, float height, const UIRectStyle& style,
                         const glm::vec4& color);
    void drawText(std::string_view text, float x, float y, float scale, const glm::vec4& color);

    [[nodiscard]] const std::vector<UIVertex>& vertices() const { return m_vertices; }
    [[nodiscard]] const std::vector<uint32_t>& indices() const { return m_indices; }
    [[nodiscard]] bool empty() const { return m_indices.empty(); }
    [[nodiscard]] UIBounds bounds() const { return m_bounds; }
    [[nodiscard]] uint32_t viewportWidth() const { return m_viewportWidth; }
    [[nodiscard]] uint32_t viewportHeight() const { return m_viewportHeight; }
    [[nodiscard]] uint32_t textItemCount() const { return m_textItems; }
    [[nodiscard]] bool truncated() const { return m_truncated; }

private:
    void addQuad(float x, float y, float width, float height, const UIRectStyle& style,
                 const glm::vec4& color);

    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    UIBounds m_bounds;
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    uint32_t m_textItems = 0;
    bool m_truncated = false;
};

// Vulkan side of the UI overlay: dedicated pipeline (no descriptors,
// no depth test, alpha blending) + persistently mapped dynamic
// vertex/index buffers sized to the UIBatch caps. record() uploads
// the accumulated batch and draws it in a single indexed draw call,
// then clears the batch for the next frame. Must be recorded inside
// the active render pass, after the scene draws.
class ENGINE_GRAPHICS_API UIRenderer {
public:
    UIRenderer(VulkanContext& context, const Swapchain& swapchain);
    ~UIRenderer();

    UIRenderer(const UIRenderer&) = delete;
    UIRenderer& operator=(const UIRenderer&) = delete;
    UIRenderer(UIRenderer&&) = delete;
    UIRenderer& operator=(UIRenderer&&) = delete;

    [[nodiscard]] UIBatch& batch() { return m_batch; }
    [[nodiscard]] const UIBatch& batch() const { return m_batch; }

    // Flushes the batch (one draw call) and clears it. No-op without
    // geometry or with a zero extent, but still clears the batch.
    void record(VkCommandBuffer commandBuffer, VkExtent2D extent);

    // The single vertex-input contract shared by the pipeline and
    // the tests: binding 0 (per-vertex), stride sizeof(UIVertex),
    // six attributes in field order.
    struct VertexInput {
        VkVertexInputBindingDescription binding{};
        std::array<VkVertexInputAttributeDescription, 6> attributes{};
    };
    [[nodiscard]] static VertexInput vertexInput();

private:
    void createPipeline(const Swapchain& swapchain);
    void createBuffers();
    [[nodiscard]] VkShaderModule createShaderModule(const char* fileName) const;

    VulkanContext& m_context;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    UIBatch m_batch;
};

} // namespace engine
