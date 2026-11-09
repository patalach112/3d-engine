#include "Core/Application.h"

#include "Physics/PhysicsEvents.h"
#include "Scene/Systems/TransformSystem.h"

#include "Core/Logger.h"
#include "Graphics/HudFont.h"
#include "Geometry/Cube.h"
#include "Scene/Components.h"
#include "Scene/Systems/CameraSystem.h"
#include "Scene/Systems/PhysicsSystem.h"
#include "Scene/Systems/PlayerSystem.h"
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
    // Phase 17 wiring: one event bus shared by scene lifetime and
    // physics events, plus the named input action the variable update
    // turns into an impulse on the picked body.
    m_scene.setEventBus(&m_eventBus);
    m_physicsSystem.setEventBus(&m_eventBus);
    m_input.bindAction("Impulse", Key::F);
    m_triggerLogSubscription = m_eventBus.subscribeScoped<TriggerEvent>([](const TriggerEvent& event) {
        ENGINE_LOG_INFO("Event: entity #" << entt::to_integral(event.otherEntity)
                                          << (event.entered ? " entered trigger #" : " exited trigger #")
                                          << entt::to_integral(event.triggerEntity));
    });
    m_collisionLogSubscription = m_eventBus.subscribeScoped<CollisionEvent>([](const CollisionEvent& event) {
        if (event.impulse > 25.0f) {
            ENGINE_LOG_INFO("Event: collision #"
                            << entt::to_integral(event.entityA) << " <-> #" << entt::to_integral(event.entityB)
                            << " impact impulse " << event.impulse << " N*s");
        }
    });
    m_destroyedLogSubscription =
        m_eventBus.subscribeScoped<EntityDestroyedEvent>([](const EntityDestroyedEvent& event) {
            ENGINE_LOG_DEBUG("Event: entity #" << entt::to_integral(event.entity) << " destroyed");
        });
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

    // Distinct sandbox palette (Assets/makeSandbox*Material): the floor,
    // falling cubes, stacking cubes, environment, and player accent each
    // get their own base color and specular/rim response, so objects can
    // never visually merge the way the old single slate-blue material let
    // them. Geometry still carries one white tint; all per-object color
    // and lighting character lives in these materials.
    const auto registerSandboxMaterial = [this](const MaterialDesc& desc) {
        const MaterialId id = m_assets.registerMaterial(desc);
        // Deterministic PSO creation per material (deduplicated by
        // pipeline state + shader pair, accelerated by the VkPipelineCache).
        if (const MaterialAsset* material = m_assets.material(id)) {
            m_pipeline.getOrCreatePipeline(material->id, material->desc.pipeline, material->desc.vertexShader,
                                           material->desc.fragmentShader, m_swapchain);
        }
        return id;
    };

    MaterialDesc defaultMaterial = makeSandboxCubeMaterial();
    defaultMaterial.name = "default"; // fallback for unassigned meshes
    registerSandboxMaterial(defaultMaterial);
    registerSandboxMaterial(makeSandboxFloorMaterial());
    registerSandboxMaterial(makeSandboxCubeMaterial());
    registerSandboxMaterial(makeSandboxStackMaterial());
    registerSandboxMaterial(makeSandboxEnvironmentMaterial());
    registerSandboxMaterial(makeSandboxPlayerMaterial());

    MaterialDesc doubleSidedMaterial = makeSandboxEnvironmentMaterial();
    doubleSidedMaterial.name = "doubleSided";
    doubleSidedMaterial.pipeline.cullMode = CullMode::None;
    registerSandboxMaterial(doubleSidedMaterial);

    // Mouse-picking highlight: a hot orange the sandbox palette never
    // uses, so the selected entity is unmistakable. Applied by swapping
    // a MeshComponent's materialName (see handleMousePicking).
    MaterialDesc highlightMaterial = makeSandboxCubeMaterial();
    highlightMaterial.name = "pickedHighlight";
    highlightMaterial.baseColor = glm::vec4{1.0f, 0.45f, 0.05f, 1.0f};
    registerSandboxMaterial(highlightMaterial);
    ENGINE_LOG_INFO("Assets registered: " << m_assets.meshCount() << " mesh(es), " << m_assets.materialCount()
                                          << " material(s), VRAM reserved " << m_assets.reservedVramBytes()
                                          << " bytes, " << m_pipeline.cachedPipelineCount()
                                          << " distinct pipeline(s)");
}

void Application::buildScene() {
    // Player: the active first-person camera riding a physical capsule.
    // The camera config position is the eye position; the capsule center
    // sits one eye-height below it. The capsule (radius 0.30 m, total
    // height 1.50 m) is simulated by PhysicsSystem, so the eye can never
    // clip through the floor, walls, or cubes the capsule cannot pass.
    Entity player = m_scene.createEntity("Player");
    player.addComponent<CameraComponent>(m_config.camera);
    auto& playerTransform = player.addComponent<TransformComponent>(
        m_config.camera.startPosition - glm::vec3{0.0f, PlayerSystem::kEyeHeight, 0.0f});
    playerTransform.scale = glm::vec3{1.0f};
    auto& playerBody = player.addComponent<RigidBodyComponent>(80.0f);
    playerBody.restitution = 0.0f;
    playerBody.friction = 0.25f;
    playerBody.setPrincipalInertia(glm::vec3{10000.0f}); // capsule stays upright
    playerBody.allowSleep = false; // the player must always respond to input instantly
    auto& playerCollider = player.addComponent<ColliderComponent>(
        ColliderComponent::capsule(PlayerSystem::kCapsuleRadius, PlayerSystem::kCapsuleHalfHeight));
    playerCollider.layer = CollisionLayer::Player; // mask stays All: the player still touches everything

    if (m_config.physics.demoMode == PhysicsDemoMode::Space) {
        // Zero-G sandbox: the uniform field is disabled for this scene and
        // the planet entity supplies localized radial gravity instead.
        m_config.physics.gravity = glm::vec3{0.0f};
        m_config.physics.gravitationalParameter = 0.0f;
        playerBody.useGravity = false; // free-flying physical player in zero-G

        Entity planet = m_scene.createEntity("RadialGravityPlanet");
        auto& planetTransform = planet.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -5.0f});
        planetTransform.scale = glm::vec3{1.6f};
        auto& planetMesh = planet.addComponent<MeshComponent>("cube");
        planetMesh.materialName = "environment";
        auto& planetBody = planet.addComponent<RigidBodyComponent>();
        planetBody.setStatic(true);
        planetBody.friction = 0.4f;
        planet.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));
        auto& planetGravity = planet.addComponent<GravityFieldComponent>();
        planetGravity.gravitationalParameter = 20.0f;
        planetGravity.radius = 0.0f; // unbounded inverse-square field

        const auto addSpaceBody = [this](const char* name, const glm::vec3& position, float scale,
                                         const glm::vec3& velocity, const ColliderComponent& collider,
                                         const char* materialName = "cube") {
            Entity entity = m_scene.createEntity(name);
            auto& transform = entity.addComponent<TransformComponent>(position);
            transform.scale = glm::vec3{scale};
            auto& mesh = entity.addComponent<MeshComponent>("cube");
            mesh.materialName = materialName;
            auto& body = entity.addComponent<RigidBodyComponent>(1.0f);
            body.velocity = velocity;
            body.restitution = 0.2f;
            body.friction = 0.3f;
            entity.addComponent<ColliderComponent>(collider);
            return entity;
        };

        // Tangential velocity sqrt(mu/r) = sqrt(20/2.5) ~= 2.83 m/s.
        addSpaceBody("OrbitalCraft", glm::vec3{-2.5f, 0.0f, -5.0f}, 0.45f, glm::vec3{0.0f, 2.83f, 0.0f},
                     ColliderComponent::capsule(0.3f, 0.35f), "player");

        Entity drone = addSpaceBody("ThrusterDrone", glm::vec3{2.2f, 1.2f, -5.0f}, 0.35f,
                                    glm::vec3{-0.5f, 0.0f, 0.0f}, ColliderComponent::sphere(0.5f));
        auto& thruster = drone.addComponent<ThrusterComponent>();
        thruster.force = glm::vec3{2.5f, 0.0f, 0.0f};
        thruster.localPosition = glm::vec3{0.0f, -0.25f, 0.0f}; // offset -> torque
        thruster.throttle = 1.0f;

        Entity projectile = addSpaceBody("FastProjectile", glm::vec3{-4.0f, -1.2f, -5.0f}, 0.18f,
                                         glm::vec3{35.0f, 0.0f, 0.0f}, ColliderComponent::sphere(0.5f),
                                         "player");
        projectile.getComponent<RigidBodyComponent>().useGravity = false;

        Entity wall = m_scene.createEntity("ThinCcdWall");
        auto& wallTransform = wall.addComponent<TransformComponent>(glm::vec3{0.0f, -1.2f, -5.0f});
        wallTransform.scale = glm::vec3{0.1f, 2.0f, 2.0f};
        auto& wallMesh = wall.addComponent<MeshComponent>("cube");
        wallMesh.materialName = "environment";
        auto& wallBody = wall.addComponent<RigidBodyComponent>();
        wallBody.setStatic(true);
        wall.addComponent<ColliderComponent>(ColliderComponent::obb(glm::vec3{0.5f}));

        Entity cargoA = addSpaceBody("JointCargoA", glm::vec3{1.8f, -1.8f, -4.0f}, 0.3f,
                                     glm::vec3{0.0f}, ColliderComponent::sphere(0.5f));
        Entity cargoB = addSpaceBody("JointCargoB", glm::vec3{2.6f, -1.8f, -4.0f}, 0.3f,
                                     glm::vec3{0.0f}, ColliderComponent::sphere(0.5f));
        cargoA.getComponent<RigidBodyComponent>().useGravity = false;
        cargoB.getComponent<RigidBodyComponent>().useGravity = false;
        auto& cargoJoint = cargoA.addComponent<JointComponent>();
        cargoJoint.other = cargoB.handle();
        cargoJoint.type = JointType::Distance;
        cargoJoint.restLength = 0.8f;

        Entity springCraft = addSpaceBody("SpringAnchoredCraft", glm::vec3{-1.0f, 2.0f, -4.0f}, 0.3f,
                                          glm::vec3{0.4f, 0.0f, 0.0f}, ColliderComponent::sphere(0.5f),
                                          "stackCube");
        springCraft.getComponent<RigidBodyComponent>().useGravity = false;
        auto& spring = springCraft.addComponent<JointComponent>();
        spring.type = JointType::SpringDamper;
        spring.anchorB = glm::vec3{-1.0f, 3.0f, -4.0f}; // world anchor (other is null)
        spring.restLength = 1.0f;
        spring.stiffness = 20.0f;
        spring.damping = 2.0f;

        m_physicsSystem.captureSpawnStates(m_scene);
        ENGINE_LOG_INFO("Scene built: " << m_scene.entityCount()
                                        << " entities (zero-G orbital sandbox: radial-gravity planet, "
                                        << "capsule craft, thruster drone, jointed cargo, spring craft, "
                                        << "fast CCD projectile and thin OBB wall)");
        return;
    }

    // Static floor: rendered as a flattened cube whose top surface is
    // y=-1.25. Its box collider is derived from the same transform scale,
    // so the physical floor is exactly the rendered 9 x 0.1 x 9 slab —
    // bodies (including the player capsule) fall at its visible edges.
    constexpr float floorTop = -1.25f;
    Entity floor = m_scene.createEntity("Floor");
    auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, floorTop - 0.05f, 0.0f});
    floorTransform.scale = glm::vec3{9.0f, 0.1f, 9.0f};
    auto& floorMesh = floor.addComponent<MeshComponent>("cube");
    floorMesh.materialName = "floor";
    auto& floorBody = floor.addComponent<RigidBodyComponent>();
    floorBody.setStatic(true);
    floorBody.restitution = 0.35f;
    floorBody.friction = 0.6f;
    floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

    // Falling/bouncing rigid-body cubes. Their unit-cube mesh is scaled
    // by TransformComponent; the box collider's local half-extents are
    // 0.5, so PhysicsSystem derives each world AABB from that same scale.
    const auto addPhysicsCube = [this](const char* name, const glm::vec3& position, float scale,
                                       const glm::vec3& velocity, const glm::vec3& angularVelocity,
                                       const char* materialName = "cube") {
        Entity entity = m_scene.createEntity(name);
        auto& transform = entity.addComponent<TransformComponent>(position);
        transform.scale = glm::vec3{scale};
        auto& mesh = entity.addComponent<MeshComponent>("cube");
        mesh.materialName = materialName;
        auto& body = entity.addComponent<RigidBodyComponent>(scale * scale * scale);
        body.velocity = velocity;
        body.angularVelocity = angularVelocity;
        body.restitution = 0.38f;
        body.friction = 0.5f;
        entity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        return entity;
    };

    Entity centerCube = addPhysicsCube("CubeCenter", glm::vec3{0.0f, 1.4f, -2.4f}, 0.65f,
                                       glm::vec3{0.15f, 0.0f, 0.0f}, glm::vec3{0.0f, 24.0f, 0.0f});

    // Scene-graph showcase (Phase 18): a small visual cube parented
    // to the tumbling physics cube. It carries no body or collider —
    // TransformSystem composes its world matrix from the parent's
    // physics-driven transform every step, so it rides and spins
    // with the cube exactly.
    Entity beacon = m_scene.createEntity("CubeCenterBeacon");
    auto& beaconTransform = beacon.addComponent<TransformComponent>(glm::vec3{0.0f, 0.75f, 0.0f});
    beaconTransform.scale = glm::vec3{0.35f};
    auto& beaconMesh = beacon.addComponent<MeshComponent>("cube");
    beaconMesh.materialName = "player";
    m_scene.setParent(beacon, centerCube);
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

    // High-stress terrestrial stack: twelve resting unit-mass cubes in a
    // 3x4 arrangement behind the falling cubes. Warm starting and island
    // solving keep this load settled instead of jittering apart.
    for (int level = 0; level < 4; ++level) {
        for (int column = 0; column < 3; ++column) {
            addPhysicsCube("StackCube",
                           glm::vec3{-0.55f + static_cast<float>(column) * 0.58f +
                                         static_cast<float>(level) * 0.03f,
                                     floorTop + 0.26f + static_cast<float>(level) * 0.53f, -3.6f},
                           0.5f, glm::vec3{0.0f}, glm::vec3{0.0f}, "stackCube");
        }
    }

    // Trigger pad (Phase 17): a static trigger volume beside the
    // stacks. Walking or falling through it publishes TriggerEvents
    // on the bus (logged by the ctor hooks); it has no contact
    // response, so bodies pass straight through.
    Entity triggerPad = m_scene.createEntity("TriggerPad");
    auto& padTransform = triggerPad.addComponent<TransformComponent>(glm::vec3{2.6f, floorTop + 0.15f, -1.0f});
    padTransform.scale = glm::vec3{1.2f, 0.3f, 1.2f};
    auto& padMesh = triggerPad.addComponent<MeshComponent>("cube");
    padMesh.materialName = "environment";
    auto& padBody = triggerPad.addComponent<RigidBodyComponent>();
    padBody.setStatic(true);
    auto& padCollider = triggerPad.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
    padCollider.isTrigger = true;

    // Respawn snapshots for the C key. Capturing handles + values (not
    // component pointers) makes repeated resets allocation-free and safe.
    m_physicsSystem.captureSpawnStates(m_scene);

    ENGINE_LOG_INFO("Scene built: " << m_scene.entityCount() << " entities (player camera, static physics floor, "
                                    << "6 falling rigid-body cubes, 12-cube stacking stress scene, "
                                    << "physical capsule player)");
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
                    "(click window to capture mouse, Esc releases / quits), left-click selects the object "
                    "under the cursor (crosshair when captured), F launches the selected body");

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
        // Propagate the scene graph first so physics sees current
        // parent placements (Phase 18).
        TransformSystem::update(m_scene);
        // Rigid-body physics is deterministic simulation, so it lives in
        // the fixed step (never the variable/render-rate update). Its
        // parallel phases return at JobSystem barriers before rendering.
        // Player input becomes rigid-body velocity first, then physics
        // resolves gravity, floor contact, cube pushback, and sliding.
        PlayerSystem::update(m_scene, m_input, fixedDeltaSeconds);
        m_physicsSystem.update(m_scene, fixedDeltaSeconds, m_config.physics, m_jobSystem);
        PlayerSystem::syncCamera(m_scene);
        RotationSystem::update(m_scene, fixedDeltaSeconds, m_jobSystem);
        // Re-propagate after every writer of local transforms so
        // picking and rendering consume this step's world matrices.
        TransformSystem::update(m_scene);
    };
    // Player control is frame-rate dependent by nature (mouse look), so
    // the camera system lives in the variable update.
    callbacks.update = [this](double deltaSeconds) {
        handleMouseCapture();
        handlePhysicsReset();
        handleEventImpulse();
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

void Application::handleMousePicking() {
    // Edge trigger: a held button must not re-pick every frame.
    if (!m_input.wasMouseButtonPressed(MouseButton::Left)) {
        return;
    }
    CameraComponent* activeCamera = m_scene.findActiveCamera();
    if (activeCamera == nullptr) {
        return;
    }
    const float aspectRatio =
        m_window.height() > 0 ? static_cast<float>(m_window.width()) / static_cast<float>(m_window.height()) : 1.0f;
    // With the cursor captured there is no visible pointer position:
    // pick through the screen centre (the crosshair). Otherwise pick
    // through the actual cursor position.
    const glm::vec2 ndc =
        m_input.isMouseCaptured()
            ? glm::vec2{0.0f, 0.0f}
            : screenToNdc(m_input.mousePosition(),
                          glm::vec2{static_cast<float>(m_window.width()), static_cast<float>(m_window.height())});
    const Ray ray = cameraRay(activeCamera->camera, ndc, aspectRatio);

    auto& registry = m_scene.registry();
    // Restore the previously selected entity's material before choosing
    // a new one, so at most one object ever wears the highlight.
    if (m_selectedEntity != entt::null && registry.valid(m_selectedEntity)) {
        if (MeshComponent* mesh = registry.try_get<MeshComponent>(m_selectedEntity);
            mesh != nullptr && mesh->materialName == "pickedHighlight") {
            mesh->materialName = m_selectedMaterialName;
        }
    }
    m_selectedEntity = entt::null;
    m_selectedMaterialName.clear();

    RaycastHit hit;
    const Scene::MeshResolver resolveMesh = [this](std::string_view assetId) { return m_assets.mesh(assetId); };
    if (!m_scene.raycastPrecise(ray, hit, resolveMesh)) {
        ENGINE_LOG_DEBUG("Mouse pick: nothing under the cursor");
        return;
    }
    m_selectedEntity = hit.entity;
    std::string pickedName = "entity";
    if (const TagComponent* tag = registry.try_get<TagComponent>(hit.entity); tag != nullptr) {
        pickedName = tag->tag;
    }
    if (MeshComponent* mesh = registry.try_get<MeshComponent>(hit.entity); mesh != nullptr) {
        m_selectedMaterialName = mesh->materialName;
        mesh->materialName = "pickedHighlight";
    }
    ENGINE_LOG_INFO("Mouse pick: selected '" << pickedName << "' (#" << entt::to_integral(hit.entity)
                                             << ") at distance " << hit.distance << " m, position ("
                                             << hit.position.x << ", " << hit.position.y << ", " << hit.position.z
                                             << "), normal (" << hit.normal.x << ", " << hit.normal.y << ", "
                                             << hit.normal.z << ")");
}

void Application::handleEventImpulse() {
    // Input action -> gameplay effect, end to end through the Phase 17
    // stack: the "Impulse" action (bound to F in the ctor) pops the
    // Phase 13 picked body up and away from the camera.
    if (!m_input.wasActionPressed("Impulse") || m_selectedEntity == entt::null) {
        return;
    }
    auto& registry = m_scene.registry();
    if (!registry.valid(m_selectedEntity)) {
        return;
    }
    RigidBodyComponent* body = registry.try_get<RigidBodyComponent>(m_selectedEntity);
    if (body == nullptr || !body->dynamic()) {
        ENGINE_LOG_INFO("Impulse action: selected entity is not a dynamic body");
        return;
    }
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    if (CameraComponent* camera = m_scene.findActiveCamera(); camera != nullptr) {
        direction += camera->camera.front() * 0.75f;
    }
    body->velocity += glm::normalize(direction) * 6.0f;
    body->sleeping = false;
    body->sleepTimer = 0.0f;
    ENGINE_LOG_INFO("Impulse action: launched entity #" << entt::to_integral(m_selectedEntity));
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
