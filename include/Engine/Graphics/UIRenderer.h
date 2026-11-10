#pragma once
#include "Core/Export.h"

#include <vulkan/vulkan.h>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Graphics/Buffer.h"
#include "Graphics/SdfFont.h"

namespace engine {

class VulkanContext;
class Swapchain;

// ---------------------------------------------------------------------------
// UIVertex / UIBatch / UIRenderer (Phase 26)
//
// Immediate-mode screen-space UI drawn by one analytic-shader
// pipeline (shaders/ui.vert + ui.frag). Three primitive kinds
// share the batch: analytic shapes (rounded-box SDF: rounded
// rectangles, borders, solid colors), sampled images (texture *
// tint, SDF-clipped) and SDF text (baked distance-field atlas,
// crisp at any scale). UIBatch is the pure-CPU accumulator
// (headless-testable); UIRenderer uploads the per-frame batch into
// persistently mapped dynamic buffers and flushes it inside the
// present render pass, after the 3D scene pass: consecutive quads
// sharing scissor rect and texture form one segment = one indexed
// draw call, so a plain shape batch still flushes as a single
// draw, while scissor changes and texture switches split segments
// exactly where the Vulkan state must change.
//
// Coordinates are pixels with a top-left origin (y down). uv spans
// 0..1 across each quad (the shader recovers the rect's exact
// pixel size from the uv derivatives); textured kinds carry an
// atlas rect in uvRect. Textures are referenced by small integer
// ids so the batch stays Vulkan-free: kUIFontTexture is the
// renderer's baked SDF atlas, custom ids come from
// UIRenderer::registerTexture() with a ready-made descriptor set
// (icons, thumbnails, render targets...).
// ---------------------------------------------------------------------------

// Primitive kind tags stored in UIVertex::kind.
inline constexpr float kUIShapePrimitive = 0.0f;
inline constexpr float kUIImagePrimitive = 1.0f;
inline constexpr float kUITextPrimitive = 2.0f;

using UITextureId = uint32_t;
inline constexpr UITextureId kUINoTexture = 0;   // analytic shapes
inline constexpr UITextureId kUIFontTexture = 1; // UIRenderer's SDF atlas

struct UIVertex {
    glm::vec2 position{0.0f, 0.0f};     // pixels, top-left origin
    glm::vec2 uv{0.0f, 0.0f};           // 0..1 across the quad
    glm::vec4 color{1.0f};              // fill RGBA / textured tint
    float cornerRadius = 0.0f;          // pixels (clamped to half the short side)
    float borderWidth = 0.0f;           // pixels, 0 = no border
    glm::vec4 borderColor{0.0f};        // RGBA, used when borderWidth > 0
    glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f}; // atlas (u0, v0, u1, v1); full range for shapes
    float kind = kUIShapePrimitive;     // kUIShapePrimitive / kUIImagePrimitive / kUITextPrimitive
};

struct UIRectStyle {
    float cornerRadius = 0.0f;   // pixels
    float borderWidth = 0.0f;    // pixels, 0 = no border
    glm::vec4 borderColor{0.0f}; // RGBA, used when borderWidth > 0
};

// Axis-aligned clip rectangle in pixels (top-left origin).
struct UIScissorRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool operator==(const UIScissorRect&) const = default;
};

// One flush segment: a run of indices sharing scissor + texture
// state, drawn with a single vkCmdDrawIndexed.
struct UISegment {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t scissor = 0; // index into UIBatch::scissors()
    UITextureId texture = kUINoTexture;

    bool operator==(const UISegment&) const = default;
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
// overflowing (truncated() reports it). A push/pop scissor stack
// gives nested clipping: the effective scissor is the intersection
// of the whole stack, entry 0 of scissors() is always the full
// viewport. drawText lays out real SDF glyph quads once a font
// atlas is set (UIRenderer does this for its own batch); without
// an atlas it degrades to the Step 1 accounting stub.
class ENGINE_GRAPHICS_API UIBatch {
public:
    static constexpr uint32_t kMaxVertices = 65536;
    static constexpr uint32_t kMaxIndices = 98304; // 1.5x quads' worth

    void begin(uint32_t viewportWidth, uint32_t viewportHeight);
    void clear();

    // Font atlas used by drawText/measureText. Not owned; must
    // outlive the batch. Survives begin()/clear().
    void setFontAtlas(const SdfFontAtlas* atlas) { m_fontAtlas = atlas; }
    [[nodiscard]] const SdfFontAtlas* fontAtlas() const { return m_fontAtlas; }

    void drawRect(float x, float y, float width, float height, const glm::vec4& color);
    void drawRoundedRect(float x, float y, float width, float height, float cornerRadius,
                         const glm::vec4& color);
    void drawRoundedRect(float x, float y, float width, float height, const UIRectStyle& style,
                         const glm::vec4& color);
    // Sampled image (kind 1): full texture, or an explicit atlas
    // sub-rect (u0, v0, u1, v1). `texture` comes from
    // UIRenderer::registerTexture().
    void drawTexture(UITextureId texture, float x, float y, float width, float height,
                     const glm::vec4& tint);
    void drawTexture(UITextureId texture, float x, float y, float width, float height,
                     const glm::vec4& uvRect, const glm::vec4& tint);
    // SDF text (kind 2) with (x, y) at the first line's glyph-box
    // top-left; '\n' starts a new line. scale multiplies font
    // pixels into screen pixels. Without a font atlas this only
    // accounts the item (stub behavior).
    void drawText(std::string_view text, float x, float y, float scale, const glm::vec4& color);
    // Width of the widest line in screen pixels (0 without a font
    // atlas or for empty text).
    [[nodiscard]] float measureText(std::string_view text, float scale) const;

    // Nested clipping: push intersects `rect` with the current
    // effective scissor; pop restores the previous one (popping the
    // root viewport scissor is a no-op).
    void pushScissor(float x, float y, float width, float height);
    void popScissor();
    [[nodiscard]] UIScissorRect currentScissor() const;
    [[nodiscard]] uint32_t scissorDepth() const { return static_cast<uint32_t>(m_scissorStack.size()); }

    [[nodiscard]] const std::vector<UIVertex>& vertices() const { return m_vertices; }
    [[nodiscard]] const std::vector<uint32_t>& indices() const { return m_indices; }
    [[nodiscard]] const std::vector<UISegment>& segments() const { return m_segments; }
    [[nodiscard]] const std::vector<UIScissorRect>& scissors() const { return m_scissors; }
    [[nodiscard]] bool empty() const { return m_indices.empty(); }
    [[nodiscard]] UIBounds bounds() const { return m_bounds; }
    [[nodiscard]] uint32_t viewportWidth() const { return m_viewportWidth; }
    [[nodiscard]] uint32_t viewportHeight() const { return m_viewportHeight; }
    [[nodiscard]] uint32_t textItemCount() const { return m_textItems; }
    [[nodiscard]] bool truncated() const { return m_truncated; }

private:
    void addQuad(float x, float y, float width, float height, const UIRectStyle& style,
                 const glm::vec4& color, float kind, UITextureId texture, const glm::vec4& uvRect);

    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<UISegment> m_segments;
    std::vector<UIScissorRect> m_scissors{UIScissorRect{}}; // append-only; [0] = viewport
    std::vector<uint32_t> m_scissorStack{0}; // indices into m_scissors
    UIBounds m_bounds;
    const SdfFontAtlas* m_fontAtlas = nullptr;
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    uint32_t m_textItems = 0;
    bool m_truncated = false;
};

// Vulkan side of the UI overlay: dedicated pipeline (one combined
// image sampler descriptor, no depth test, alpha blending) +
// persistently mapped dynamic vertex/index buffers sized to the
// UIBatch caps + the baked SDF font atlas texture. record()
// uploads the accumulated batch and flushes it segment by segment
// (vkCmdSetScissor + descriptor bind + one indexed draw per
// segment), restores the full-frame scissor for the passes that
// follow, then clears the batch. Must be recorded inside the
// active render pass, after the scene draws.
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
    [[nodiscard]] const SdfFontAtlas& fontAtlas() const { return m_fontAtlas; }

    // Registers a caller-owned descriptor set (binding 0 must be a
    // combined image sampler compatible with the UI pipeline) for
    // drawTexture(). The set stays owned by the caller and must
    // outlive its last recorded frame. Ids are never reused.
    UITextureId registerTexture(VkDescriptorSet descriptorSet);
    void unregisterTexture(UITextureId texture);

    // Flushes the batch and clears it. No-op without geometry or
    // with a zero extent, but still clears the batch.
    void record(VkCommandBuffer commandBuffer, VkExtent2D extent);

    // The single vertex-input contract shared by the pipeline and
    // the tests: binding 0 (per-vertex), stride sizeof(UIVertex),
    // eight attributes in field order.
    struct VertexInput {
        VkVertexInputBindingDescription binding{};
        std::array<VkVertexInputAttributeDescription, 8> attributes{};
    };
    [[nodiscard]] static VertexInput vertexInput();

private:
    void createPipeline(const Swapchain& swapchain);
    void createBuffers();
    void createFontAtlasTexture();
    void createFontDescriptorSet();
    [[nodiscard]] VkShaderModule createShaderModule(const char* fileName) const;
    [[nodiscard]] VkDescriptorSet resolveDescriptorSet(UITextureId texture) const;

    VulkanContext& m_context;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_fontDescriptorSet = VK_NULL_HANDLE;
    VkImage m_atlasImage = VK_NULL_HANDLE;
    VkDeviceMemory m_atlasMemory = VK_NULL_HANDLE;
    VkImageView m_atlasView = VK_NULL_HANDLE;
    VkSampler m_atlasSampler = VK_NULL_HANDLE;
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    SdfFontAtlas m_fontAtlas;
    UIBatch m_batch;
    std::unordered_map<UITextureId, VkDescriptorSet> m_textures;
    UITextureId m_nextTexture = kUIFontTexture + 1;
};

} // namespace engine
