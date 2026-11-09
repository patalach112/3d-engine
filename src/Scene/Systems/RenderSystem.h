#pragma once

#include "Graphics/RenderQueue.h" // DrawItem + RenderQueue

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>

namespace engine {

class JobSystem;
class Renderer;
class Scene;
class Swapchain;

// Everything needed to draw one frame, derived purely from Scene state.
// Kept separate from RenderSystem::render() so the scene -> matrices
// step can be unit-tested without a GPU.
struct PreparedFrame {
    bool hasCamera = false;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    RenderQueue renderQueue; // draw items, sorted ready for submission
    uint32_t meshCount = 0;   // visible mesh entities considered
    uint32_t culledCount = 0; // of those, removed by frustum culling
};

// Knobs for frame preparation.
struct FramePrepareOptions {
    JobSystem* jobs = nullptr;      // parallelize matrix/bounds work when set
    bool frustumCulling = true;     // from [Render] config
    bool sortDrawItems = true;      // sort the render queue before submission
    size_t parallelMinItems = 128;  // below this, sequential is cheaper
};

// ---------------------------------------------------------------------------
// RenderSystem
//
// Bridges the ECS world and the Vulkan renderer: finds the active camera,
// culls mesh entities whose world bounds fall outside the view frustum,
// builds model matrices for the survivors, and aggregates them into a
// RenderQueue sorted by material, then front-to-back depth (minimizing
// GPU state changes and maximizing Early-Z rejection). Model/bounds
// computation is data-parallel over entities when a JobSystem is
// provided (disjoint writes into pre-sized slots), and the queue sort
// parallelizes through the same JobSystem for large frames.
// ---------------------------------------------------------------------------
class RenderSystem {
public:
    RenderSystem(Renderer& renderer, const Swapchain& swapchain, JobSystem* jobs = nullptr,
                 bool frustumCulling = true, size_t parallelMinItems = 128, bool sortDrawItems = true);

    // Draws the current scene state. Must run after update systems so it
    // never renders a half-updated world.
    void render(Scene& scene);

    // Scene -> frame data derivation (no GPU involvement).
    static PreparedFrame prepareFrame(Scene& scene, float aspectRatio,
                                      const FramePrepareOptions& options = {});

    // Statistics of the most recent render() call, for loop logging.
    [[nodiscard]] uint32_t lastDrawnCount() const { return m_lastDrawnCount; }
    [[nodiscard]] uint32_t lastCulledCount() const { return m_lastCulledCount; }

private:
    Renderer& m_renderer;
    const Swapchain& m_swapchain;
    JobSystem* m_jobs;
    bool m_frustumCulling;
    size_t m_parallelMinItems;
    bool m_sortDrawItems;
    bool m_warnedNoCamera = false;
    uint32_t m_lastDrawnCount = 0;
    uint32_t m_lastCulledCount = 0;
};

} // namespace engine
