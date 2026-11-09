#pragma once

#include "Core/Camera.h"
#include "Core/EngineConfig.h"
#include "Core/GameLoop.h"
#include "Graphics/GraphicsPipeline.h"
#include "Graphics/Renderer.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Platform/Input.h"
#include "Platform/Window.h"

namespace engine {

// ---------------------------------------------------------------------------
// Application
//
// Composition root of the engine. Owns every subsystem by value and relies
// on member declaration order for both construction and destruction:
//
//   construct:  config -> Window -> Input -> VulkanContext -> swapchain ->
//               GraphicsPipeline -> Renderer -> GameLoop -> Camera
//   destruct:   exact reverse, so GPU resources are always released before
//               the device, surface, instance and window they depend on.
//               (Input unregisters itself from the Window while the Window
//               is still alive.)
//
// All tuning (window, presentation, logging, loop rates, camera, input)
// arrives through EngineConfig — this class contains no hardcoded settings.
// ---------------------------------------------------------------------------
class Application {
public:
    explicit Application(EngineConfig config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    void run();

private:
    [[nodiscard]] UniformBufferObject computeTransforms() const;
    void handleMouseCapture();
    void logLoopStats(const char* context) const;

    EngineConfig m_config;
    Window m_window;
    Input m_input;
    VulkanContext m_context;
    Swapchain m_swapchain;
    GraphicsPipeline m_pipeline;
    Renderer m_renderer;
    GameLoop m_gameLoop;
    Camera m_camera;

    double m_secondsSinceStatsLog = 0.0;
};

} // namespace engine
