#include "Core/Application.h"

#include "Core/Logger.h"
#include "Scene/Components.h"
#include "Scene/Systems/CameraSystem.h"
#include "Scene/Systems/RotationSystem.h"

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
      m_jobSystem(JobSystem::Config{m_config.performance.jobWorkerCount}),
      m_renderSystem(m_renderer, m_swapchain, &m_jobSystem, m_config.render.frustumCulling,
                     m_config.performance.parallelMinDrawItems) {
    // Reserve pooled component storage before populating the scene.
    m_scene.reserveCapacity(m_config.performance.entityReserve);
    buildScene();
    if (m_config.input.captureMouseOnStart) {
        m_input.setMouseCaptured(true);
    }
}

Application::~Application() {
    // Make sure no frame is still using the resources that member
    // destruction is about to release (Renderer, buffers, swapchain...).
    m_context.waitIdle();
}

void Application::buildScene() {
    // Player: the active first-person camera.
    Entity player = m_scene.createEntity("Player");
    player.addComponent<CameraComponent>(m_config.camera);

    // The original Phase 1 cube, now an ECS entity at the origin.
    Entity cube = m_scene.createEntity("Cube");
    cube.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, 0.0f});
    cube.addComponent<MeshComponent>("cube");

    // Two companions demonstrating multi-entity rendering and the
    // fixed-step rotation system (they spin deterministically).
    Entity leftCube = m_scene.createEntity("CubeLeft");
    auto& leftTransform = leftCube.addComponent<TransformComponent>(glm::vec3{-2.0f, 0.5f, -2.0f});
    leftTransform.scale = glm::vec3{0.6f};
    leftCube.addComponent<MeshComponent>("cube");
    leftCube.addComponent<SpinComponent>(glm::vec3{20.0f, 45.0f, 0.0f});

    Entity rightCube = m_scene.createEntity("CubeRight");
    auto& rightTransform = rightCube.addComponent<TransformComponent>(glm::vec3{2.0f, -0.5f, -2.5f});
    rightTransform.scale = glm::vec3{0.8f};
    rightCube.addComponent<MeshComponent>("cube");
    rightCube.addComponent<SpinComponent>(glm::vec3{0.0f, -30.0f, 15.0f});

    ENGINE_LOG_INFO("Scene built: " << m_scene.entityCount() << " entities (player camera, "
                                    << "1 static cube, 2 spinning cubes)");
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
    // Deterministic simulation systems run at the fixed rate; rotation
    // integration is data-parallel across worker threads, and
    // parallelFor()'s return is the barrier before the variable update.
    callbacks.fixedUpdate = [this](double fixedDeltaSeconds) {
        RotationSystem::update(m_scene, fixedDeltaSeconds, m_jobSystem);
    };
    // Player control is frame-rate dependent by nature (mouse look), so
    // the camera system lives in the variable update.
    callbacks.update = [this](double deltaSeconds) {
        handleMouseCapture();
        CameraSystem::update(m_scene, m_input, deltaSeconds);

        if (m_config.loop.statsLogIntervalSeconds > 0.0) {
            m_secondsSinceStatsLog += deltaSeconds;
            if (m_secondsSinceStatsLog >= m_config.loop.statsLogIntervalSeconds) {
                m_secondsSinceStatsLog = 0.0;
                logLoopStats("Loop stats");
            }
        }
    };
    // Rendering always runs last, over fully updated scene state.
    callbacks.render = [this](double /*interpolationAlpha*/) { m_renderSystem.render(m_scene); };

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
    std::string cameraDescription = "no active camera";
    if (const CameraComponent* activeCamera = m_scene.findActiveCamera(); activeCamera != nullptr) {
        const glm::vec3& position = activeCamera->camera.position();
        cameraDescription = "camera at (" + std::to_string(position.x) + ", " + std::to_string(position.y) +
                            ", " + std::to_string(position.z) + "), yaw " +
                            std::to_string(activeCamera->camera.yawDegrees()) + ", pitch " +
                            std::to_string(activeCamera->camera.pitchDegrees());
    }
    const memory::PoolStats pool = m_scene.memoryStats();
    ENGINE_LOG_INFO(context << ": " << stats.frames << " frames in " << stats.elapsedSeconds << "s (avg "
                            << stats.averageFramesPerSecond() << " FPS, smoothed "
                            << m_gameLoop.time().framesPerSecond() << " FPS), " << stats.fixedSteps
                            << " fixed steps, " << stats.droppedSeconds * 1000.0 << " ms dropped, "
                            << m_scene.entityCount() << " entities (drew " << m_renderSystem.lastDrawnCount()
                            << ", frustum-culled " << m_renderSystem.lastCulledCount() << "), jobs completed "
                            << m_jobSystem.completedJobCount() << ", memory pool " << pool.blocksInUse << "/"
                            << pool.blocksReserved << " blocks in " << pool.pageCount << " pages, "
                            << cameraDescription);
}

} // namespace engine
