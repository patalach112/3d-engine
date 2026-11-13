#pragma once
#include "Core/Export.h"

#include "Graphics/PipelineState.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// PipelineWarmupQueue — the frame-budgeted PSO warmup plan behind
// async startup loading.
//
// vkCreateGraphicsPipelines is the single most expensive startup
// operation (driver shader compilation, seconds on a cold cache),
// and Vulkan pipeline objects can only be consumed by the thread
// recording frames — so the work is split two ways:
//
//   1. SPIR-V file reads move to JobSystem workers
//      (GraphicsPipeline::preloadShaderAsync): pure CPU/disk work
//      that never touches the main thread.
//   2. Pipeline *creation* is queued here and drained a few
//      creations per frame by the host's frame loop
//      (GraphicsPipeline::processPipelineWarmup), while the window
//      keeps pumping OS events and the renderer falls back to the
//      default pipeline for materials whose PSO has not landed yet.
//
// The queue itself is pure CPU bookkeeping (no Vulkan types), so the
// dedup / budgeting policy is unit-testable headlessly: a material
// is planned at most once (duplicate registrations are rejected),
// requests drain strictly FIFO, and a drain honours its creation
// budget exactly. Materials sharing one pipeline state still drain
// individually — GraphicsPipeline binds them to the shared PSO for
// free and only *distinct creations* consume the frame budget.
// ---------------------------------------------------------------------------

struct PipelineWarmupRequest {
    uint32_t materialId = 0;
    PipelineStateDesc state{};
    std::string vertexShader;
    std::string fragmentShader;
};

class ENGINE_GRAPHICS_API PipelineWarmupQueue {
public:
    // Plans one material. Returns false (and counts a rejected
    // duplicate) when the material is already queued or has already
    // been drained: replanning a material is always a host bug, and
    // silently accepting it would double-pay creation cost.
    bool enqueue(PipelineWarmupRequest request) {
        if (!m_knownMaterials.insert(request.materialId).second) {
            ++m_duplicatesRejected;
            return false;
        }
        m_queue.push_back(std::move(request));
        ++m_totalEnqueued;
        return true;
    }

    // Moves up to `maxRequests` queued requests (FIFO) into `out`
    // and returns how many were moved. A zero budget moves nothing.
    size_t drain(size_t maxRequests, std::vector<PipelineWarmupRequest>& out) {
        const size_t count = std::min(maxRequests, m_queue.size());
        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        return count;
    }

    // Drops every queued request (the materials stay "known": a
    // host clearing the plan mid-startup must re-register through
    // the synchronous path, not requeue behind itself).
    void clear() { m_queue.clear(); }

    [[nodiscard]] bool empty() const { return m_queue.empty(); }
    [[nodiscard]] bool complete() const { return m_queue.empty(); }
    [[nodiscard]] size_t pendingCount() const { return m_queue.size(); }
    [[nodiscard]] bool known(uint32_t materialId) const {
        return m_knownMaterials.find(materialId) != m_knownMaterials.end();
    }
    [[nodiscard]] size_t totalEnqueued() const { return m_totalEnqueued; }
    [[nodiscard]] size_t duplicatesRejected() const { return m_duplicatesRejected; }

private:
    std::deque<PipelineWarmupRequest> m_queue;
    std::unordered_set<uint32_t> m_knownMaterials;
    size_t m_totalEnqueued = 0;
    size_t m_duplicatesRejected = 0;
};

} // namespace engine
