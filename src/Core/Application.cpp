#include "Core/Application.h"

#include "Core/Logger.h"
#include "Graphics/HudFont.h"
#include "Geometry/Cube.h"
#include "Scene/Components.h"
#include "Scene/Systems/CameraSystem.h"
#include "Scene/Systems/PhysicsSystem.h"
#include "Scene/Systems/RotationSystem.h"

#include <utility>

namespace engine {

Application::Application(EngineConfig config)
    : m_config(std::move(config)),
      m_window(m_config.window.width, m_config.window.height, m_config.window.title.c_str(),
               m_config.window.resizable, m_config.input.captureMouseOnStart),
      m_input(m_window),
      m_context(m_window),
      m_swapchain(m_context, m_window,
                  PresentationSettings{m_config.render.presentMode, m_config.render.vsync}),
      m_pipeline(m_context, m_swapchain),
      m_renderer(m_context, m_window, m_swapchain, m_pipeline,
                 Renderer::framesInFlightForPresentMode(m_swapchain.presentMode())),
      m_gameLoop(m_config.loop),
      m_jobSystem(JobSystem::Config{m_config.performance.jobWorkerCount,
                                    m_config.performance.parallelBatchSize}),
      m_vramBudget(static_cast<uint64_t>(m_config.render.vramBudgetMiB) * 1024 * 1024),
      m_stagingBudget(static_cast<uint64_t>(m_config.render.stagingBufferBudgetMiB) * 1024 * 1024),
      m_assets(m_vramBudget, m_stagingBudget),
      m_renderSystem(m_renderer, m_swapchain, &m_jobSystem, m_config.render.frustumCulling,
                     m_config.performance.parallelMinDrawItems, m_config.render.sortDrawItems, &m_assets) {
    ENGINE_LOG_INFO("Hardware profile: JobSystem workers "
                    << m_jobSystem.workerCount() << " (+ main thread), parallel batch "
                    << m_config.performance.parallelBatchSize << " items, VRAM budget "
                    << m_config.render.vramBudgetMiB << " MiB (staging "
                    << m_config.render.stagingBufferBudgetMiB << " MiB)");
    registerAssets();
    // Reserve pooled component storage before populating the scene.
    m_scene.reserveCapacity(m_config.performance.entityReserve);
    buildScene();
    // No late cursor-capture call is needed here: Window already applied
    // GLFW_CURSOR_DISABLED during construction when captureMouseOnStart
    // was enabled, and Input adopted that state.
}

Application::~Application() {
    // Make sure no frame is still using the resources that member
    // destruction is about to release (Renderer, buffers, swapchain...).
    m_context.waitIdle();
}

void Application::registerAssets() {
    // Built-in geometry, registered once at startup. Registration
    // reserves VRAM against the GTX 970 budget; ids are stable from here
    // on and are what MeshComponents reference by name.
    m_assets.registerMesh("cube", Cube::vertices(), Cube::indices());

    MaterialDesc defaultMaterial;
    defaultMaterial.name = "default";
    const MaterialId defaultId = m_assets.registerMaterial(defaultMaterial);

    MaterialDesc doubleSidedMaterial;
    doubleSidedMaterial.name = "doubleSided";
    doubleSidedMaterial.pipeline.cullMode = CullMode::None;
    const MaterialId doubleSidedId = m_assets.registerMaterial(doubleSidedMaterial);

    // Deterministic PSO creation for both materials (deduplicated by
    // pipeline state + shader pair, accelerated by the VkPipelineCache).
    if (const MaterialAsset* material = m_assets.material(defaultId)) {
        m_pipeline.getOrCreatePipeline(material->id, material->desc.pipeline, material->desc.vertexShader,
                                       material->desc.fragmentShader, m_swapchain);
    }
    if (const MaterialAsset* material = m_assets.material(doubleSidedId)) {
        m_pipeline.getOrCreatePipeline(material->id, material->desc.pipeline, material->desc.vertexShader,
                                       material->desc.fragmentShader, m_swapchain);
    }
    ENGINE_LOG_INFO("Assets registered: " << m_assets.meshCount() << " mesh(es), " << m_assets.materialCount()
                                          << " material(s), VRAM reserved " << m_assets.reservedVramBytes()
                                          << " bytes, " << m_pipeline.cachedPipelineCount()
                                          << " distinct pipeline(s)");
}

void Application::buildScene() {
    // Player: the active first-person camera.
    Entity player = m_scene.createEntity("Player");
    player.addComponent<CameraComponent>(m_config.camera);

    // Static floor: rendered as a flattened cube whose top surface is
    // y=-1.25, with an infinite plane collider at exactly that height.
    constexpr float floorTop = -1.25f;
    Entity floor = m_scene.createEntity("Floor");
    auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, floorTop - 0.05f, -2.0f});
    floorTransform.scale = glm::vec3{9.0f, 0.1f, 9.0f};
    floor.addComponent<MeshComponent>("cube");
    auto& floorBody = floor.addComponent<RigidBodyComponent>();
    floorBody.setStatic(true);
    floorBody.restitution = 0.35f;
    floorBody.friction = 0.6f;
    floor.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, floorTop));

    // Falling/bouncing rigid-body cubes. Their unit-cube mesh is scaled
    // by TransformComponent; the box collider's local half-extents are
    // 0.5, so PhysicsSystem derives each world AABB from that same scale.
    const auto addPhysicsCube = [this](const char* name, const glm::vec3& position, float scale,
                                       const glm::vec3& velocity, const glm::vec3& angularVelocity) {
        Entity entity = m_scene.createEntity(name);
        auto& transform = entity.addComponent<TransformComponent>(position);
        transform.scale = glm::vec3{scale};
        entity.addComponent<MeshComponent>("cube");
        auto& body = entity.addComponent<RigidBodyComponent>(scale * scale * scale);
        body.velocity = velocity;
        body.angularVelocity = angularVelocity;
        body.restitution = 0.38f;
        body.friction = 0.5f;
        entity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        return entity;
    };

    addPhysicsCube("CubeCenter", glm::vec3{0.0f, 1.4f, -2.4f}, 0.65f, glm::vec3{0.15f, 0.0f, 0.0f},
                   glm::vec3{0.0f, 24.0f, 0.0f});
    addPhysicsCube("CubeCenterTop", glm::vec3{0.12f, 2.8f, -2.35f}, 0.65f, glm::vec3{0.0f, 0.0f, 0.0f},
                   glm::vec3{0.0f, -18.0f, 0.0f});
    addPhysicsCube("CubeLeft", glm::vec3{-1.25f, 1.9f, -2.2f}, 0.55f, glm::vec3{0.25f, 0.0f, 0.0f},
                   glm::vec3{0.0f, 32.0f, 0.0f});
    addPhysicsCube("CubeLeftTop", glm::vec3{-1.18f, 3.1f, -2.15f}, 0.55f, glm::vec3{0.0f, 0.0f, 0.0f},
                   glm::vec3{0.0f, -26.0f, 0.0f});
    addPhysicsCube("CubeRight", glm::vec3{1.3f, 1.6f, -2.1f}, 0.5f, glm::vec3{-0.2f, 0.0f, 0.0f},
                   glm::vec3{0.0f, 20.0f, 0.0f});
    addPhysicsCube("CubeRightTop", glm::vec3{1.22f, 2.75f, -2.05f}, 0.5f, glm::vec3{0.0f, 0.0f, 0.0f},
                   glm::vec3{0.0f, -30.0f, 0.0f});

    // Respawn snapshots for the C key. Capturing handles + values (not
    // component pointers) makes repeated resets allocation-free and safe.
    m_physicsSystem.captureSpawnStates(m_scene);

    ENGINE_LOG_INFO("Scene built: " << m_scene.entityCount() << " entities (player camera, static physics floor, "
                                    << "6 falling rigid-body cubes)");
}

void Application::run() {
    const LoopConfig& loop = m_config.loop;
    ENGINE_LOG_INFO("Engine started (fixed timestep "
                    << loop.fixedDeltaSeconds << "s, max " << loop.maxFixedStepsPerFrame
                    << " steps/frame, max FPS " << (loop.maxFps == 0 ? "uncapped" : std::to_string(loop.maxFps))
                    << ", present mode requested " << presentationModeName(m_config.render.presentMode)
                    << " (vsync " << (m_config.render.vsync ? "on" : "off") << "), using "
                    << Swapchain::presentModeName(m_swapchain.presentMode()) << ", "
                    << m_renderer.framesInFlight() << " frame(s) in flight)");
    ENGINE_LOG_INFO("Controls: WASD move, Q/E down/up, Shift boost, C resets physics cubes, mouse look "
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
        // Rigid-body physics is deterministic simulation, so it lives in
        // the fixed step (never the variable/render-rate update). Its
        // parallel phases return at JobSystem barriers before rendering.
        m_physicsSystem.update(m_scene, fixedDeltaSeconds, m_config.physics, m_jobSystem);
        RotationSystem::update(m_scene, fixedDeltaSeconds, m_jobSystem);
    };
    // Player control is frame-rate dependent by nature (mouse look), so
    // the camera system lives in the variable update.
    callbacks.update = [this](double deltaSeconds) {
        handleMouseCapture();
        handlePhysicsReset();
        CameraSystem::update(m_scene, m_input, deltaSeconds);

        if (m_config.loop.statsLogIntervalSeconds > 0.0) {
            m_secondsSinceStatsLog += deltaSeconds;
            if (m_secondsSinceStatsLog >= m_config.loop.statsLogIntervalSeconds) {
                m_secondsSinceStatsLog = 0.0;
                logLoopStats("Loop stats");
            }
        }

        // FPS HUD: refresh a few times per second (text re-layout is the
        // only cost; per-frame recording is one bind + one draw).
        if (m_config.render.hudEnabled) {
            m_secondsSinceHudUpdate += deltaSeconds;
            if (m_secondsSinceHudUpdate >= m_config.render.hudUpdateIntervalSeconds) {
                m_secondsSinceHudUpdate = 0.0;
                const auto& time = m_gameLoop.time();
                m_renderer.setHudText(formatHudText(
                    time.framesPerSecond(), time.deltaSeconds() * 1000.0, m_jobSystem.workerCount(),
                    m_vramBudget.usedBytes() / (1024 * 1024), m_config.render.vramBudgetMiB,
                    m_renderSystem.lastDrawnCount(), m_renderSystem.lastCulledCount()));
            }
        }
    };
    // Rendering always runs last, over fully updated scene state. The
    // return value lets GameLoop count only presented frames for FPS.
    callbacks.render = [this](double /*interpolationAlpha*/) { return m_renderSystem.render(m_scene); };

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

void Application::handlePhysicsReset() {
    // wasKeyPressed is an edge trigger: holding C must not reset every
    // frame. The reset only rewrites existing component values, so it
    // cannot create entities, reallocate ECS storage, or leak resources.
    if (!m_input.wasKeyPressed(Key::C)) {
        return;
    }
    const size_t resetCount = m_physicsSystem.resetToSpawnStates(m_scene);
    ENGINE_LOG_INFO("Physics reset (C): respawned " << resetCount << " dynamic rigid bodies");
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
