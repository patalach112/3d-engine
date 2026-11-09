#pragma once

#include "Graphics/Renderer.h" // DrawItem

#include <glm/mat4x4.hpp>

#include <vector>

namespace engine {

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
    std::vector<DrawItem> drawItems;
};

// ---------------------------------------------------------------------------
// RenderSystem
//
// Bridges the ECS world and the Vulkan renderer: finds the active camera,
// builds per-entity model matrices from TransformComponents, and submits
// them (with the camera's view/projection) to the Renderer. The Renderer
// stays a dumb GPU front-end; all scene knowledge lives here.
// ---------------------------------------------------------------------------
class RenderSystem {
public:
    RenderSystem(Renderer& renderer, const Swapchain& swapchain);

    // Draws the current scene state. Must run after update systems so it
    // never renders a half-updated world.
    void render(Scene& scene);

    // Scene -> frame data derivation (no GPU involvement).
    static PreparedFrame prepareFrame(Scene& scene, float aspectRatio);

private:
    Renderer& m_renderer;
    const Swapchain& m_swapchain;
    bool m_warnedNoCamera = false;
};

} // namespace engine
