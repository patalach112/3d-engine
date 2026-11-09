#pragma once

#include "Graphics/GraphicsPipeline.h"
#include "Graphics/Renderer.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Platform/Window.h"

namespace engine {

// ---------------------------------------------------------------------------
// Application
//
// Composition root of the engine. Owns every subsystem by value and relies
// on member declaration order for both construction and destruction:
//
//   construct:  Window -> VulkanContext -> swapchain -> GraphicsPipeline
//               -> Renderer
//   destruct:   exact reverse, so GPU resources are always released before
//               the device, surface, instance and window they depend on.
//
// Phase 1 scope: run the game loop and render one rotating colored cube.
// ---------------------------------------------------------------------------
class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    void run();

private:
    [[nodiscard]] UniformBufferObject computeTransforms(float elapsedSeconds) const;

    Window m_window;
    VulkanContext m_context;
    Swapchain m_swapchain;
    GraphicsPipeline m_pipeline;
    Renderer m_renderer;
};

} // namespace engine
