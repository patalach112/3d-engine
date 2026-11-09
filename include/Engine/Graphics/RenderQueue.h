#pragma once
#include "Core/Export.h"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine {

class JobSystem;

// Size of the material texture array bound at descriptor binding 2
// (must match cube.frag). Draw items carry indices into it; -1 means
// "no texture, use the scalar factor".
inline constexpr uint32_t kMaxMaterialTextures = 32;

// One drawable entity for a frame, in CPU-side submission order.
//
// `sortKey` packs the opaque-render sort criteria into a single integer
// (see RenderQueue::makeSortKey): material/pipeline identity first so
// consecutive draws share GPU state, then quantized view depth ascending
// so nearer geometry draws first and the depth buffer rejects occluded
// fragments as early as possible (Early-Z). `materialId` and `viewDepth`
// are kept alongside for inspection, tooling, and tests.
struct DrawItem {
    glm::mat4 model{1.0f};
    uint64_t sortKey = 0;
    uint32_t materialId = 0;
    // Slot of this material in the descriptor-indexing material array
    // (AssetManager assigns dense indices). Carried per draw so shaders
    // can fetch material data by index instead of rebinding descriptors.
    uint32_t descriptorIndex = 0;
    float viewDepth = 0.0f;
    // Uniform material color for this draw (from MaterialDesc::baseColor).
    // Pushed to the shader with the model matrix so every sandbox mesh is
    // shaded as one solid material, not per-face debug colors.
    glm::vec4 baseColor{1.0f};
    // Packed material lighting (from MaterialDesc::lighting): specular
    // color in rgb, shininess in a.
    glm::vec4 specular{0.35f, 0.35f, 0.35f, 32.0f};
    // Surface response: x = specular strength, y = rim strength,
    // z = rim power, w = ambient strength.
    glm::vec4 surface{0.35f, 0.10f, 3.0f, 0.28f};
    // Metallic-roughness factors (Phase 22): x = metallic,
    // y = roughness, z = occlusion strength, w unused.
    glm::vec4 pbrParams{0.0f, 0.55f, 1.0f, 0.0f};
    // Material texture array slots: albedo, metallicRoughness,
    // normal, occlusion. -1 = untextured slot.
    std::array<int32_t, 4> textureIndices{-1, -1, -1, -1};
};

// Push-constant payload for one draw: model matrix, material base
// color, metallic-roughness factors, and material texture indices.
// 112 bytes, inside Vulkan's guaranteed 128-byte push-constant
// minimum, so richer per-material PBR data costs no descriptor
// updates and no extra draw calls.
struct DrawPushConstants {
    glm::mat4 model{1.0f};
    glm::vec4 baseColor{1.0f};
    glm::vec4 pbrParams{0.0f, 0.55f, 1.0f, 0.0f}; // x metallic, y roughness, z occlusion strength
    glm::ivec4 textureIndices{-1, -1, -1, -1};    // albedo, metallicRoughness, normal, occlusion
};
static_assert(sizeof(DrawPushConstants) == 112, "DrawPushConstants must stay a compact 112-byte payload");
static_assert(offsetof(DrawPushConstants, model) == 0 && offsetof(DrawPushConstants, baseColor) == 64 &&
                  offsetof(DrawPushConstants, pbrParams) == 80 && offsetof(DrawPushConstants, textureIndices) == 96,
              "DrawPushConstants layout must match the shader push-constant block");

// ---------------------------------------------------------------------------
// RenderQueue
//
// CPU-side render command stream: aggregates the frame's DrawItems and
// sorts them before they are recorded into a Vulkan command buffer, so
// the GPU sees minimal pipeline/material state changes and maximal
// Early-Z rejection. Pure CPU data — no Vulkan objects involved.
//
// Allocation behavior: reserve() preallocates both the item storage and
// the sort scratch buffer; clear() keeps both capacities, and sorting
// never allocates. Sorting is stable, so equal keys keep collection
// order and the output is fully deterministic:
//
//   * sort()            — sequential stable LSD radix sort over the
//                         64-bit key (8 passes, O(n)).
//   * sort(jobs, min)   — when a JobSystem is given and the queue is
//                         large enough: per-segment sorts run through
//                         JobSystem::parallelFor, then stable merge
//                         stages (also via parallelFor) combine them.
//                         Below the threshold it falls back to sort().
// ---------------------------------------------------------------------------
class ENGINE_GRAPHICS_API RenderQueue {
public:
    // Packs (material, depth) into the 64-bit opaque sort key. Depth is
    // clamped to the positive range; for non-negative floats the IEEE
    // bit pattern is order-isomorphic, so integer key order == depth
    // order.
    [[nodiscard]] static uint64_t makeSortKey(uint32_t materialId, float viewDepth);

    void clear() noexcept { m_items.clear(); }
    void reserve(size_t capacity);
    void push(const DrawItem& item) { m_items.push_back(item); }
    void push(DrawItem&& item) { m_items.push_back(std::move(item)); }

    [[nodiscard]] std::span<const DrawItem> items() const { return m_items; }
    [[nodiscard]] size_t size() const { return m_items.size(); }
    [[nodiscard]] bool empty() const { return m_items.empty(); }

    void sort();
    void sort(JobSystem* jobs, size_t parallelMinItems);

private:
    void ensureScratch();

    std::vector<DrawItem> m_items;
    std::vector<DrawItem> m_scratch;
};

} // namespace engine
