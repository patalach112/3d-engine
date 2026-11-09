#include "Core/Application.h"

#include "Core/Logger.h"

#include <glm/gtc/matrix_transform.hpp>

#include <utility>

namespace engine {

Application::Application(EngineConfig config)
    : m_config(std::move(config)),
      m_window(m_config.window.width, m_config.window.height, m_config.window.title.c_str(),
               m_config.window.resizable),
      m_context(m_window),
      m_swapchain(m_context, m_window, m_config.render.vsync),
      m_pipeline(m_context, m_swapchain),
      m_renderer(m_context, m_window, m_swapchain, m_pipeline),
      m_gameLoop(m_config.loop) {}

Application::~Application() {
    // Make sure no frame is still using the resources that member
    // destruction is about to release (Renderer, buffers, swapchain...).
    m_context.waitIdle();
}

void Application::run() {
    const LoopConfig& loop = m_config.loop;
    ENGINE_LOG_INFO("Engine started (fixed timestep "
                    << loop.fixedDeltaSeconds << "s, max " << loop.maxFixedStepsPerFrame
                    << " steps/frame, max FPS " << (loop.maxFps == 0 ? "uncapped" : std::to_string(loop.maxFps))
                    << ", vsync " << (m_config.render.vsync ? "on" : "off") << ")");

    LoopCallbacks callbacks;
    callbacks.shouldContinue = [this] { return !m_window.shouldClose(); };
    callbacks.pollEvents = [this] {
        m_window.pollEvents();
        if (m_window.isEscapePressed()) {
            m_window.requestClose();
        }
    };
    callbacks.fixedUpdate = [this](double fixedDeltaSeconds) { m_cubeRotation.advance(fixedDeltaSeconds); };
    callbacks.update = [this](double deltaSeconds) {
        // Variable-rate hook: periodic loop statistics when configured.
        if (m_config.loop.statsLogIntervalSeconds > 0.0) {
            m_secondsSinceStatsLog += deltaSeconds;
            if (m_secondsSinceStatsLog >= m_config.loop.statsLogIntervalSeconds) {
                m_secondsSinceStatsLog = 0.0;
                logLoopStats("Loop stats");
            }
        }
    };
    callbacks.render = [this](double interpolationAlpha) {
        m_renderer.drawFrame(computeTransforms(interpolationAlpha));
    };

    m_gameLoop.run(callbacks);

    // Drain the GPU before teardown so destructors never free resources
    // that are still in flight.
    m_context.waitIdle();
    logLoopStats("Engine shutting down");
}

void Application::logLoopStats(const char* context) const {
    const LoopStats& stats = m_gameLoop.stats();
    ENGINE_LOG_INFO(context << ": " << stats.frames << " frames in " << stats.elapsedSeconds << "s (avg "
                            << stats.averageFramesPerSecond() << " FPS, smoothed "
                            << m_gameLoop.time().framesPerSecond() << " FPS), " << stats.fixedSteps
                            << " fixed steps, " << stats.droppedSeconds * 1000.0
                            << " ms of simulation time dropped");
}

void Application::CubeRotation::advance(double fixedDeltaSeconds) {
    anglesDegrees = glm::mod(anglesDegrees + angularVelocityDegreesPerSecond * static_cast<float>(fixedDeltaSeconds),
                             360.0f);
}

glm::vec3 Application::CubeRotation::extrapolatedAngles(double interpolationAlpha,
                                                        double fixedDeltaSeconds) const {
    const float extraSeconds = static_cast<float>(interpolationAlpha * fixedDeltaSeconds);
    return anglesDegrees + angularVelocityDegreesPerSecond * extraSeconds;
}

UniformBufferObject Application::computeTransforms(double interpolationAlpha) const {
    UniformBufferObject ubo;

    // Orientation from the fixed-step simulation, extrapolated by the
    // fraction of a step that has accumulated since the last step.
    const glm::vec3 angles =
        m_cubeRotation.extrapolatedAngles(interpolationAlpha, m_config.loop.fixedDeltaSeconds);
    glm::mat4 model{1.0f};
    model = glm::rotate(model, glm::radians(angles.y), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(angles.x), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(angles.z), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.model = model;

    // Static camera looking at the origin from +Z.
    ubo.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                           glm::vec3(0.0f, 1.0f, 0.0f));

    const VkExtent2D extent = m_swapchain.extent();
    const float aspect = extent.height > 0
                             ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                             : 1.0f;
    ubo.proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);

    // GLM was designed for OpenGL, whose clip-space Y axis points down
    // compared to Vulkan. Flip it, otherwise the image is upside down
    // (the pipeline's clockwise front-face setting compensates winding).
    ubo.proj[1][1] *= -1.0f;

    return ubo;
}

} // namespace engine
