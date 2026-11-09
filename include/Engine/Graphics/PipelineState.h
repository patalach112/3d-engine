#pragma once
#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace engine {

// ---------------------------------------------------------------------------
// Pipeline state description (PSO key material).
//
// Engine-level (Vulkan-independent) description of the fixed-function
// state that distinguishes one graphics pipeline from another. Materials
// carry one of these; the PSO cache hashes the description to a stable
// key, and the Vulkan layer maps the enums to Vk* values when creating
// the actual VkPipeline. Deterministic field order + hashing makes PSO
// identity reproducible across runs and machines.
// ---------------------------------------------------------------------------
enum class PrimitiveTopology : uint8_t { TriangleList, TriangleStrip, LineList };
enum class PolygonMode : uint8_t { Fill, Line };
enum class CullMode : uint8_t { None, Back, Front };
enum class DepthCompare : uint8_t { Less, LessOrEqual, Equal, Always, Never };

struct PipelineStateDesc {
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    PolygonMode polygonMode = PolygonMode::Fill;
    CullMode cullMode = CullMode::Back;
    bool depthTestEnable = true;
    bool depthWriteEnable = true;
    DepthCompare depthCompare = DepthCompare::Less;
    bool blendEnable = false;

    bool operator==(const PipelineStateDesc&) const = default;
};

// Deterministic 64-bit hash of a pipeline state description (FNV-1a over
// the field values in declaration order).
[[nodiscard]] ENGINE_GRAPHICS_API uint64_t hashPipelineState(const PipelineStateDesc& state);

// ---------------------------------------------------------------------------
// PipelineStateCache
//
// Assigns stable PSO ids to pipeline state descriptions: interning the
// same state twice returns the same id and creates nothing new, so the
// number of real VkPipelines the Vulkan layer must build equals the
// number of distinct states, not the number of materials. Ids are
// 1-based (0 = invalid) and assigned in first-seen order — deterministic
// for a deterministic registration order.
// ---------------------------------------------------------------------------
class ENGINE_GRAPHICS_API PipelineStateCache {
public:
    uint32_t intern(const PipelineStateDesc& state);
    [[nodiscard]] uint32_t find(const PipelineStateDesc& state) const;
    [[nodiscard]] size_t size() const { return m_idsByHash.size(); }
    [[nodiscard]] size_t creationCount() const { return m_creationCount; }

private:
    std::unordered_map<uint64_t, uint32_t> m_idsByHash;
    size_t m_creationCount = 0;
};

} // namespace engine
