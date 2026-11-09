#pragma once

#include "Core/EngineConfig.h"
#include "Core/GameLoop.h"
#include "Graphics/GraphicsPipeline.h"
#include "Graphics/Renderer.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Platform/Window.h"

#include <glm/vec3.hpp>

namespace engine {

// ---------------------------------------------------------------------------
// Application
//
// Composition root of the engine. Owns every subsystem by value and relies
// on member declaration order for both construction and destruction:
//
//   construct:  config -> Window -> VulkanContext -> swapchain ->
//               GraphicsPipeline -> Renderer -> GameLoop
//   destruct:   exact reverse, so GPU resources are always released before
//               the device, surface, instance and window they depend on.
//
// All tuning (window, presentation, logging, loop rates) arrives through
// EngineConfig — this class contains no hardcoded settings.
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
    // Phase 1 demo scene state. The cube's orientation is integrated in
    // fixed updates (deterministic), and rendering extrapolates it by the
    // loop's interpolation alpha so motion stays smooth at any frame rate.
    struct CubeRotation {
        glm::vec3 anglesDegrees{0.0f};
        glm::vec3 angularVelocityDegreesPerSecond{55.0f, 32.0f, 18.0f}; // X, Y, Z

        void advance(double fixedDeltaSeconds);
        [[nodiscard]] glm::vec3 extrapolatedAngles(double interpolationAlpha,
                                                   double fixedDeltaSeconds) const;
    };

    [[nodiscard]] UniformBufferObject computeTransforms(double interpolationAlpha) const;
    void logLoopStats(const char* context) const;

    EngineConfig m_config;
    Window m_window;
    VulkanContext m_context;
    Swapchain m_swapchain;
    GraphicsPipeline m_pipeline;
    Renderer m_renderer;
    GameLoop m_gameLoop;

    CubeRotation m_cubeRotation;
    double m_secondsSinceStatsLog = 0.0;
};

} // namespace engine
