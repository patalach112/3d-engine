#include "Core/Application.h"

#include "Core/Logger.h"

#include <glm/gtc/matrix_transform.hpp>

#include <utility>

namespace engine {

Application::Application(EngineConfig config)
    : m_config(std::move(config)),
      m_window(m_config.window.width, m_config.window.height, m_config.window.title.c_str(),
               m_config.window.resizable),
      m_input(m_window),
      m_context(m_window),
      m_swapchain(m_context, m_window, m_config.render.vsync),
      m_pipeline(m_context, m_swapchain),
      m_renderer(m_context, m_window, m_swapchain, m_pipeline),
      m_gameLoop(m_config.loop),
      m_camera(m_config.camera) {
    if (m_config.input.captureMouseOnStart) {
        m_input.setMouseCaptured(true);
    }
}

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
    ENGINE_LOG_INFO("Controls: WASD move, Q/E down/up, Shift boost, mouse look "
                    "(click window to capture mouse, Esc releases / quits)");

    LoopCallbacks callbacks;
    callbacks.shouldContinue = [this] { return !m_window.shouldClose(); };
    callbacks.pollEvents = [this] {
        // Input frame protocol: clear per-frame state, then let GLFW
        // callbacks refill it during polling.
        m_input.beginFrame();
        m_window.pollEvents();
    };
    // First-person controls are frame-rate dependent by nature (mouse look),
    // so they live in the variable update, integrated with delta time.
    callbacks.update = [this](double deltaSeconds) {
        handleMouseCapture();

        if (m_input.isMouseCaptured()) {
            m_camera.processLook(m_input.mouseDelta());
        }
        m_camera.processMovement(m_input, deltaSeconds);

        if (m_config.loop.statsLogIntervalSeconds > 0.0) {
            m_secondsSinceStatsLog += deltaSeconds;
            if (m_secondsSinceStatsLog >= m_config.loop.statsLogIntervalSeconds) {
                m_secondsSinceStatsLog = 0.0;
                logLoopStats("Loop stats");
            }
        }
    };
    callbacks.render = [this](double /*interpolationAlpha*/) {
        m_renderer.drawFrame(computeTransforms());
    };

    m_gameLoop.run(callbacks);

    // Drain the GPU before teardown so destructors never free resources
    // that are still in flight.
    m_context.waitIdle();
    logLoopStats("Engine shutting down");
}

void Application::handleMouseCapture() {
    if (m_input.wasKeyPressed(Key::Escape)) {
        if (m_input.isMouseCaptured()) {
            m_input.setMouseCaptured(false);
            ENGINE_LOG_DEBUG("Mouse released (Esc)");
        } else {
            m_window.requestClose();
        }
        return;
    }
    if (!m_input.isMouseCaptured() && m_config.input.captureMouseOnClick &&
        m_input.wasMouseButtonPressed(MouseButton::Left)) {
        m_input.setMouseCaptured(true);
        ENGINE_LOG_DEBUG("Mouse captured (click)");
    }
}

void Application::logLoopStats(const char* context) const {
    const LoopStats& stats = m_gameLoop.stats();
    const glm::vec3& position = m_camera.position();
    ENGINE_LOG_INFO(context << ": " << stats.frames << " frames in " << stats.elapsedSeconds << "s (avg "
                            << stats.averageFramesPerSecond() << " FPS, smoothed "
                            << m_gameLoop.time().framesPerSecond() << " FPS), " << stats.fixedSteps
                            << " fixed steps, " << stats.droppedSeconds * 1000.0
                            << " ms dropped; camera at (" << position.x << ", " << position.y << ", "
                            << position.z << "), yaw " << m_camera.yawDegrees() << ", pitch "
                            << m_camera.pitchDegrees());
}

UniformBufferObject Application::computeTransforms() const {
    UniformBufferObject ubo;

    // The cube is static now; the player moves around it.
    ubo.model = glm::mat4{1.0f};
    ubo.view = m_camera.viewMatrix();

    const VkExtent2D extent = m_swapchain.extent();
    const float aspect = extent.height > 0
                             ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                             : 1.0f;
    ubo.proj = m_camera.projectionMatrix(aspect);

    return ubo;
}

} // namespace engine
