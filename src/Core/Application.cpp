#include "Core/Application.h"

#include "Physics/PhysicsEvents.h"

#include <algorithm>
#include <filesystem>
#include "Scene/Systems/TransformSystem.h"

#include "Core/Logger.h"
#include "Graphics/HudFont.h"
#include "Graphics/UIInspector.h"
#include "Scene/ComponentReflection.h"
#include "Scene/Raycast.h"
#include "Scene/SceneBinarySerialization.h"
#include "Geometry/Cube.h"
#include "Scene/Components.h"
#include "Scene/ProjectExport.h"
#include "Scene/Systems/CameraSystem.h"
#include "Scene/NativeScript.h"
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
                     m_config.performance.parallelMinDrawItems, m_config.render.sortDrawItems, &m_assets,
                     m_config.render.occlusionCulling) {
    m_renderer.setPostSettings(PostSettings{static_cast<float>(m_config.render.exposure),
                                            m_config.render.bloomEnabled,
                                            static_cast<float>(m_config.render.bloomStrength),
                                            static_cast<float>(m_config.render.bloomThreshold),
                                            m_config.render.skyboxEnabled,
                                            static_cast<float>(m_config.render.environmentIntensity)});

    // Native game module (Phase 23): optional — the engine runs
    // unchanged without one. The manager logs its own outcome.
    if (!m_config.game.modulePath.empty()) {
        EngineContext context = buildModuleContext(0.0f, 0.0f);
        m_moduleManager.load(m_config.game.modulePath, context);
    }

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
    // Phase 19 startup proof: serialize the freshly built scene and
    // reload it into a scratch scene. The live scene (and the
    // physics spawn snapshots captured from it) is never touched --
    // this only proves the document round-trips losslessly.
    const std::string sceneSnapshot = m_scene.saveToString();
    Scene scratchScene;
    if (scratchScene.loadFromString(sceneSnapshot) && scratchScene.entityCount() == m_scene.entityCount()) {
        ENGINE_LOG_INFO("Scene serialization round-trip verified at startup (" << m_scene.entityCount()
                                                                               << " entities, "
                                                                               << sceneSnapshot.size() << " bytes of JSON)");
    } else {
        ENGINE_LOG_WARNING("Scene serialization round-trip FAILED at startup");
    }
    // Phase 20: asynchronous asset pipeline. File-backed assets in
    // ./assets (OBJ meshes, PPM textures) are declared and requested
    // here; JobSystem workers parse them while the frame loop's sync
    // boundary (processCompletedLoads in the update callback)
    // publishes finished loads, and a 1 Hz mtime poll hot-reloads
    // edited files. Names come from file stems, so a MeshComponent
    // referencing "crate" picks up assets/crate.obj when it lands.
    m_assets.setJobSystem(&m_jobSystem);
    m_assets.setAssetEventCallback([](std::string_view name, AssetKind kind, AssetState state) {
        if (state == AssetState::Loaded || state == AssetState::Failed) {
            ENGINE_LOG_INFO("Asset event: '" << name << "' (" << (kind == AssetKind::Mesh ? "mesh" : "texture")
                                             << ") -> " << (state == AssetState::Loaded ? "loaded" : "FAILED"));
        }
    });
    if (std::filesystem::is_directory("assets")) {
        for (const auto& entry : std::filesystem::directory_iterator("assets")) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string stem = entry.path().stem().string();
            const std::string extension = entry.path().extension().string();
            if (extension == ".obj" && m_assets.declareMeshSource(stem, entry.path())) {
                m_assets.requestLoad(stem);
                ENGINE_LOG_INFO("AssetManager: queued async mesh load for '" << stem << "'");
            } else if (extension == ".ppm" && m_assets.declareTextureSource(stem, entry.path())) {
                m_assets.requestLoad(stem);
                ENGINE_LOG_INFO("AssetManager: queued async texture load for '" << stem << "'");
            }
        }
    }
    // No late cursor-capture call is needed here: Window already applied
    // GLFW_CURSOR_DISABLED during construction when captureMouseOnStart
    // was enabled, and Input adopted that state.
}

Application::~Application() {
    // Retire native script instances while every member (above all
    // the module manager and its still-loaded module) is alive:
    // deleting an instance calls module code through its vtable,
    // which must never happen after the module has been unloaded.
    // Member destruction would retire the module BEFORE the scene
    // (reverse declaration order), so this cannot be left to
    // ~Scene's entity clear — by then it would be too late.
    NativeScriptSystem::shutdown(m_scene);
    // Make sure no frame is still using the resources that member
    // destruction is about to release (Renderer, buffers, swapchain...).
    m_context.waitIdle();
}

namespace {

// Phase 27 editor: inspects one reflected component of `entity`
// (when present) in a panel stacked at (x, y); returns the y just
// below the panel so components pile up in the inspector dock.
template <typename T>
float inspectComponentAt(UIContext& ui, Scene::Registry& registry, entt::entity entity, float x, float y,
                         float width, const UIInspectorOptions& options) {
    if (!registry.all_of<T>(entity)) {
        return y;
    }
    const TypeInfo& type = reflect<T>();
    float rows = 1.0f; // title row
    for (const PropertyInfo& property : type.properties) {
        rows += property.type == PropertyType::Vec3 ? 3.0f : 1.0f;
    }
    const float height =
        2.0f * UIContext::kPanelPadding + rows * (UIContext::kRowHeight + UIContext::kRowSpacing);
    inspectComponent(ui, UIRect{x, y, width, height}, type, &registry.get<T>(entity), options);
    return y + height + 6.0f;
}

// Phase 32 editor: live material panel for the selected entity's
// MeshComponent. Materials are shared assets (not components), so
// the panel edits a copy and commits through
// AssetManager::updateMaterial when a widget changes it — the
// next frame's draw items shade with the new PBR values.
float inspectMaterialAt(UIContext& ui, AssetManager& assets, const MeshComponent& mesh, float x, float y,
                        float width) {
    const MaterialAsset* material = assets.material(mesh.materialName);
    if (material == nullptr) {
        return y;
    }
    MaterialDesc desc = material->desc;
    const float rows = 8.0f; // title, name, base color (3), metallic, roughness, occlusion
    const float height = 2.0f * UIContext::kPanelPadding + rows * (UIContext::kRowHeight + UIContext::kRowSpacing);
    ui.beginPanel("Material", UIRect{x, y, width, height}, UIRectStyle{8.0f, 1.0f, glm::vec4{}});
    ui.label("Material");
    ui.label(mesh.materialName);
    bool changed = false;
    glm::vec3 baseColor = glm::vec3{desc.baseColor};
    if (ui.vec3Sliders("Base Color", &baseColor, 0.0f, 1.0f)) {
        desc.baseColor = glm::vec4{baseColor, desc.baseColor.w};
        changed = true;
    }
    changed = ui.slider("Metallic", &desc.pbr.metallic, 0.0f, 1.0f) || changed;
    changed = ui.slider("Roughness", &desc.pbr.roughness, 0.0f, 1.0f) || changed;
    changed = ui.slider("Occlusion Strength", &desc.pbr.occlusionStrength, 0.0f, 1.0f) || changed;
    ui.endPanel();
    if (changed) {
        assets.updateMaterial(mesh.materialName, desc);
    }
    return y + height + 6.0f;
}

} // namespace

EngineContext Application::buildModuleContext(float deltaSeconds, float fixedDeltaSeconds) {
    EngineContext context;
    context.scene = &m_scene;
    context.input = &m_input;
    context.assets = &m_assets;
    context.jobs = &m_jobSystem;
    context.scripts = &ScriptRegistry::instance();
    context.deltaSeconds = deltaSeconds;
    context.fixedDeltaSeconds = fixedDeltaSeconds;
    context.elapsedSeconds = m_moduleElapsedSeconds;
    context.frameIndex = m_moduleFrameIndex;
    return context;
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

    // Dynamic lights (Phase 21): a warm point light hovering over
    // the sandbox and a downward spotlight aimed at the floor.
    // LightingSystem gathers them every frame; the fragment shader
    // applies smooth distance attenuation, so pools of light move
    // correctly as the camera flies around.
    Entity pointLight = m_scene.createEntity("WarmPointLight");
    pointLight.addComponent<TransformComponent>(glm::vec3{0.0f, 3.0f, -2.0f});
    auto& warmLight = pointLight.addComponent<PointLightComponent>();
    warmLight.color = glm::vec3{1.0f, 0.72f, 0.42f};
    warmLight.intensity = 42.0f;
    warmLight.range = 14.0f;

    Entity spotLight = m_scene.createEntity("FloorSpotLight");
    auto& spotTransform = spotLight.addComponent<TransformComponent>(glm::vec3{2.5f, 5.0f, -1.0f});
    spotTransform.rotationDegrees = glm::vec3{-90.0f, 0.0f, 0.0f}; // aim -Z downward
    auto& spot = spotLight.addComponent<SpotLightComponent>();
    spot.color = glm::vec3{0.75f, 0.85f, 1.0f};
    spot.intensity = 55.0f;
    spot.range = 16.0f;
    spot.innerConeRadians = 0.45f;
    spot.outerConeRadians = 0.75f;
    spot.castShadow = true; // Phase 32: real-time spot shadows

    // Directional key light (Phase 32): the sun is an entity now.
    // These values reproduce the pre-Phase-32 fixed sun exactly;
    // the Inspector can retarget, recolor or shadow-tune it live.
    Entity sun = m_scene.createEntity("Sun");
    auto& sunLight = sun.addComponent<DirectionalLightComponent>();
    sunLight.color = glm::vec3{1.0f, 0.96f, 0.88f};
    sunLight.direction = glm::vec3{0.35f, 0.8f, 0.45f};
    sunLight.intensity = 1.0f;

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
                    "under the cursor (crosshair when captured), F launches the selected body. The scene "
                    "starts frozen in Edit mode: Play (toolbar or Ctrl+P) runs the simulation, Stop "
                    "restores the pre-play snapshot");

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
        // Phase 31: the simulation (game modules, native scripts,
        // player, rigid-body physics, spin) runs only in Play
        // mode. In Edit mode the world stays frozen exactly as
        // authored; transforms still propagate so rendering,
        // picking and the gizmo see the edited placements.
        if (m_playSession.isPlaying()) {
            // Game module fixed logic (Phase 23) runs before physics so
            // it can drive bodies the solver then resolves.
            EngineContext context = buildModuleContext(0.0f, static_cast<float>(fixedDeltaSeconds));
            m_moduleManager.fixedUpdate(context, static_cast<float>(fixedDeltaSeconds));
            // Native scripts (Phase 25) tick in the fixed step too,
            // before physics, so script-driven forces are resolved
            // by the same step that produced them.
            NativeScriptSystem::fixedUpdate(context, static_cast<float>(fixedDeltaSeconds));
            // Rigid-body physics is deterministic simulation, so it lives in
            // the fixed step (never the variable/render-rate update). Its
            // parallel phases return at JobSystem barriers before rendering.
            // Player input becomes rigid-body velocity first, then physics
            // resolves gravity, floor contact, cube pushback, and sliding.
            PlayerSystem::update(m_scene, m_input, fixedDeltaSeconds);
            m_physicsSystem.update(m_scene, fixedDeltaSeconds, m_config.physics, m_jobSystem);
            PlayerSystem::syncCamera(m_scene);
            RotationSystem::update(m_scene, fixedDeltaSeconds, m_jobSystem);
        }
        // Re-propagate after every writer of local transforms so
        // picking and rendering consume this step's world matrices.
        TransformSystem::update(m_scene);
    };
    // Player control is frame-rate dependent by nature (mouse look), so
    // the camera system lives in the variable update.
    callbacks.update = [this](double deltaSeconds) {
        // Asset sync boundary (Phase 20): publish a bounded number of
        // finished async loads per frame, and poll source files for
        // hot reloads once a second.
        m_assets.processCompletedLoads(8);
        m_secondsSinceAssetPoll += deltaSeconds;
        if (m_secondsSinceAssetPoll >= 1.0) {
            m_secondsSinceAssetPoll = 0.0;
            m_assets.pollFileChanges();
        }
        // Game module (Phase 23): variable update, then the
        // half-second hot-reload poll (mtime on the source file).
        m_moduleElapsedSeconds += deltaSeconds;
        ++m_moduleFrameIndex;
        {
            EngineContext context = buildModuleContext(static_cast<float>(deltaSeconds), 0.0f);
            m_moduleManager.update(context, static_cast<float>(deltaSeconds));
            NativeScriptSystem::update(context, static_cast<float>(deltaSeconds));
            if (m_config.game.moduleHotReload) {
                m_secondsSinceModulePoll += deltaSeconds;
                if (m_secondsSinceModulePoll >= 0.5) {
                    m_secondsSinceModulePoll = 0.0;
                    m_moduleManager.pollForChanges(context);
                }
            }
        }
        handleMouseCapture();
        // The C-key respawn is a runtime action: live in Play
        // mode only (Edit mode's frozen scene has nothing to reset).
        if (m_playSession.isPlaying()) {
            handlePhysicsReset();
        }
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
    callbacks.render = [this](double /*interpolationAlpha*/) {
        // Phase 26 UI overlay: a translucent rounded backdrop panel
        // behind the FPS HUD text plus an accent rule under it. The
        // batch is per-frame (flushed and cleared by the renderer's
        // overlay record), so it is issued fresh on every frame.
        if (m_config.render.hudEnabled) {
            UIBatch& ui = m_renderer.uiBatch();
            ui.begin(static_cast<uint32_t>(std::max(m_window.width(), 0)),
                     static_cast<uint32_t>(std::max(m_window.height(), 0)));

            // Phase 26 Step 3: interactive widgets. While the mouse
            // is captured for flight the pointer is parked
            // off-screen, so the UI stays visible but inert.
            UIInput uiInput;
            if (!m_input.isMouseCaptured()) {
                const glm::vec2 mouse = m_input.mousePosition();
                uiInput.mouseX = mouse.x;
                uiInput.mouseY = mouse.y;
                uiInput.mouseDown = m_input.isMouseButtonDown(MouseButton::Left);
                uiInput.keyStep = (m_input.wasKeyPressed(Key::Right) ? 1 : 0) -
                                  (m_input.wasKeyPressed(Key::Left) ? 1 : 0);
                // Phase 27: right-click context menus + Esc dismissal.
                uiInput.mouseRightDown = m_input.isMouseButtonDown(MouseButton::Right);
                uiInput.escapePressed = m_input.wasKeyPressed(Key::Escape);
                uiInput.scrollY = m_input.scrollDelta().y;
            }
            m_uiContext.beginFrame(uiInput, ui);
            // A menu open at frame start owns this frame's clicks:
            // neither picking nor gizmo drags may start from them.
            const bool popupOpenAtFrameStart = m_uiContext.popupOpen();

            // Phase 31: in Play mode the editor's authoring surface
            // (tool keys, scene-file shortcuts, inspector, outliner,
            // gizmo, picking, asset drops) is inert — only Stop and
            // the runtime itself are live.
            const bool playing = m_playSession.isPlaying();

            // Phase 28 gizmo tool switching (also on the toolbar below).
            if (!playing && m_input.wasKeyPressed(Key::Num1)) {
                m_gizmo.setMode(GizmoMode::Translate);
            } else if (!playing && m_input.wasKeyPressed(Key::Num2)) {
                m_gizmo.setMode(GizmoMode::Rotate);
            } else if (!playing && m_input.wasKeyPressed(Key::Num3)) {
                m_gizmo.setMode(GizmoMode::Scale);
            }

            // Phase 30 scene file shortcuts: Ctrl+S saves the whole
            // registry to the .gbq quicksave, Ctrl+O restores it
            // (Edit mode only — never serialize the live runtime
            // scene over the authored quicksave). Phase 31: Ctrl+P
            // toggles Play/Stop.
            const bool ctrlHeld =
                m_input.isKeyDown(Key::LeftControl) || m_input.isKeyDown(Key::RightControl);
            if (ctrlHeld && m_input.wasKeyPressed(Key::P)) {
                if (playing) {
                    exitPlayMode();
                } else {
                    enterPlayMode();
                }
            } else if (!playing && ctrlHeld && m_input.wasKeyPressed(Key::S)) {
                saveSceneToGbq();
            } else if (!playing && ctrlHeld && m_input.wasKeyPressed(Key::O)) {
                loadSceneFromGbq();
            }

            ui.drawRoundedRect(6.0f, 6.0f, 372.0f, 118.0f,
                               UIRectStyle{10.0f, 1.0f, glm::vec4{1.0f, 1.0f, 1.0f, 0.16f}},
                               glm::vec4{0.02f, 0.03f, 0.05f, 0.55f});
            ui.drawRect(6.0f, 126.0f, 372.0f, 3.0f, glm::vec4{0.35f, 0.75f, 1.0f, 0.9f});
            // SDF text (Step 2): a control hint along the bottom edge,
            // crisp at any window size from the baked distance atlas.
            const float hintY = static_cast<float>(std::max(m_window.height() - 34, 0));
            ui.drawText(playing ? "PLAY MODE - SIMULATION RUNNING - STOP (OR CTRL+P) RESTORES THE EDIT SCENE"
                                : "WASD + MOUSE FLY - SPACE/CTRL RISE/SINK - Q/E ROLL - CTRL+P PLAYS THE SCENE",
                        12.0f, hintY, 1.5f, glm::vec4{0.75f, 0.85f, 1.0f, 0.85f});

            // Phase 28 tool toolbar: gizmo mode switch (keys 1/2/3),
            // its accent underline marking the active tool.
            const UIRect toolbarRect{386.0f, 8.0f, 246.0f, 46.0f};
            ui.drawRoundedRect(toolbarRect.x, toolbarRect.y, toolbarRect.width, toolbarRect.height,
                               UIRectStyle{8.0f, 1.0f, glm::vec4{1.0f, 1.0f, 1.0f, 0.14f}},
                               glm::vec4{0.02f, 0.03f, 0.05f, 0.55f});
            const float kToolX[3] = {394.0f, 472.0f, 550.0f};
            const int activeTool = m_gizmo.mode() == GizmoMode::Translate ? 0 : m_gizmo.mode() == GizmoMode::Rotate ? 1 : 2;
            ui.drawRect(kToolX[activeTool], 45.0f, 74.0f, 3.0f, glm::vec4{1.0f, 0.82f, 0.25f, 0.95f});
            // (Phase 31: in Play mode the authoring buttons still
            // draw, but their actions are ignored.)
            if (m_uiContext.button(UIRect{kToolX[0], 17.0f, 74.0f, 28.0f}, "Move") && !playing) {
                m_gizmo.setMode(GizmoMode::Translate);
            }
            if (m_uiContext.button(UIRect{kToolX[1], 17.0f, 74.0f, 28.0f}, "Rotate") && !playing) {
                m_gizmo.setMode(GizmoMode::Rotate);
            }
            if (m_uiContext.button(UIRect{kToolX[2], 17.0f, 74.0f, 28.0f}, "Scale") && !playing) {
                m_gizmo.setMode(GizmoMode::Scale);
            }

            // Phase 30: scene file commands beside the tool switch —
            // Save serializes the active registry to the .gbq
            // quicksave, Load clears and restores it wholesale.
            const UIRect sceneFileRect{640.0f, 8.0f, 168.0f, 46.0f};
            ui.drawRoundedRect(sceneFileRect.x, sceneFileRect.y, sceneFileRect.width, sceneFileRect.height,
                               UIRectStyle{8.0f, 1.0f, glm::vec4{1.0f, 1.0f, 1.0f, 0.14f}},
                               glm::vec4{0.02f, 0.03f, 0.05f, 0.55f});
            if (m_uiContext.button(UIRect{648.0f, 17.0f, 74.0f, 28.0f}, "Save") && !playing) {
                saveSceneToGbq();
            }
            if (m_uiContext.button(UIRect{726.0f, 17.0f, 74.0f, 28.0f}, "Load") && !playing) {
                loadSceneFromGbq();
            }

            // Phase 31: Play/Stop segment. Play snapshots the scene
            // and starts the runtime simulation; Stop tears the
            // runtime down and restores the pre-play snapshot.
            // The accent underline marks the button matching the
            // live mode (Stop stays reachable in Play mode, when
            // every other authoring control is inert).
            const UIRect playRect{816.0f, 8.0f, 168.0f, 46.0f};
            ui.drawRoundedRect(playRect.x, playRect.y, playRect.width, playRect.height,
                               UIRectStyle{8.0f, 1.0f, glm::vec4{1.0f, 1.0f, 1.0f, 0.14f}},
                               glm::vec4{0.02f, 0.03f, 0.05f, 0.55f});
            ui.drawRect(playing ? 902.0f : 824.0f, 45.0f, 74.0f, 3.0f,
                        playing ? glm::vec4{1.0f, 0.35f, 0.25f, 0.95f} : glm::vec4{0.35f, 1.0f, 0.55f, 0.95f});
            if (m_uiContext.button(UIRect{824.0f, 17.0f, 74.0f, 28.0f}, "Play")) {
                if (!playing) {
                    enterPlayMode();
                }
            }
            if (m_uiContext.button(UIRect{902.0f, 17.0f, 74.0f, 28.0f}, "Stop")) {
                if (playing) {
                    exitPlayMode();
                }
            }

            // Phase 33: "Export Game" segment. Packages the live
            // (Edit-mode) scene plus every asset it references into
            // the GBQB bundle the standalone game-player boots.
            // Inert while playing: an export always ships the
            // authored world, never mid-simulation state.
            const UIRect exportRect{992.0f, 8.0f, 92.0f, 46.0f};
            ui.drawRoundedRect(exportRect.x, exportRect.y, exportRect.width, exportRect.height,
                               UIRectStyle{8.0f, 1.0f, glm::vec4{1.0f, 1.0f, 1.0f, 0.14f}},
                               glm::vec4{0.02f, 0.03f, 0.05f, 0.55f});
            if (m_uiContext.button(UIRect{1000.0f, 17.0f, 74.0f, 28.0f}, "Export") && !playing) {
                exportGame();
            }

            // Phase 27 editor layout: the Component Inspector dock
            // (top-right) reflects the Outliner's selection over
            // every reflected built-in component; the Scene
            // Hierarchy panel (left, below the HUD backdrop) is
            // built LAST so its context menu paints above the
            // dock. With nothing selected the inspector falls back
            // to the player ship's RigidBodyComponent (the Step 3
            // behavior), present only when the flight module has
            // spawned the ship.
            auto& registry = m_scene.registry();
            UIInspectorOptions inspectorOptions;
            inspectorOptions.floatMin = 0.0f;
            inspectorOptions.floatMax = 5000.0f;
            constexpr float kInspectorWidth = 322.0f;
            const float inspectorX =
                static_cast<float>(m_window.width()) - kInspectorWidth - 8.0f;
            const entt::entity selected = m_outliner.selected();
            float inspectorBottom = 8.0f; // viewport picking occluder edge
            // Phase 31: no inspector in Play mode — its widgets
            // edit component memory, which the runtime owns.
            if (!playing && selected != entt::null && registry.valid(selected)) {
                float y = 8.0f;
                y = inspectComponentAt<TransformComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                           kInspectorWidth, inspectorOptions);
                y = inspectComponentAt<RigidBodyComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                           kInspectorWidth, inspectorOptions);
                y = inspectComponentAt<SpinComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                      kInspectorWidth, inspectorOptions);
                y = inspectComponentAt<PointLightComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                            kInspectorWidth, inspectorOptions);
                y = inspectComponentAt<SpotLightComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                           kInspectorWidth, inspectorOptions);
                y = inspectComponentAt<DirectionalLightComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                                  kInspectorWidth, inspectorOptions);
                if (registry.all_of<MeshComponent>(selected)) {
                    y = inspectMaterialAt(m_uiContext, m_assets, registry.get<MeshComponent>(selected),
                                          inspectorX, y, kInspectorWidth);
                }
                inspectorBottom = y;
            } else if (!playing) {
                const auto tags = registry.view<TagComponent>();
                for (const auto entity : tags) {
                    if (tags.get<TagComponent>(entity).tag != "Spacecraft" ||
                        !registry.all_of<RigidBodyComponent>(entity)) {
                        continue;
                    }
                    auto& body = registry.get<RigidBodyComponent>(entity);
                    inspectComponent(m_uiContext, UIRect{inspectorX, 8.0f, kInspectorWidth, 252.0f},
                                     reflect<RigidBodyComponent>(), &body, inspectorOptions);
                    inspectorBottom = 260.0f;
                    break;
                }
            }
            const float outlinerHeight =
                std::max(static_cast<float>(m_window.height()) - 148.0f, 120.0f);
            const UIRect outlinerRect{6.0f, 140.0f, 300.0f, outlinerHeight};
            const glm::vec2 viewportSize{static_cast<float>(m_window.width()),
                                         static_cast<float>(m_window.height())};
            m_outliner.build(m_uiContext, outlinerRect, m_scene);
            // Phase 29: Asset Browser dock along the bottom edge,
            // between the hierarchy (left) and inspector (right).
            const UIRect assetRect{312.0f, std::max(viewportSize.y - 196.0f, 150.0f),
                                   std::max(viewportSize.x - 650.0f, 240.0f), 188.0f};

            // Phase 28: transform gizmo over the selection, then
            // viewport picking. The gizmo runs first so a press that
            // grabs a handle never re-picks; picking then fires only
            // for presses the UI left unconsumed, outside every dock
            // (HUD, toolbar, inspector, outliner), with no context
            // menu open at frame start and no widget mid-drag.
            if (const CameraComponent* activeCamera = m_scene.findActiveCamera(); activeCamera != nullptr) {
                // Phase 31: `playing` blocks both the gizmo (never
                // updated — nothing draws or drags) and picking.
                const bool gizmoBlocked = playing || popupOpenAtFrameStart || m_input.isMouseCaptured();
                const GizmoUpdateResult gizmoResult =
                    playing ? GizmoUpdateResult{}
                            : m_gizmo.update(m_uiContext, m_scene, m_outliner.selected(), activeCamera->camera,
                                             viewportSize, gizmoBlocked);
                const glm::vec2 mouse = m_uiContext.mousePosition();
                const auto insideRect = [](const UIRect& rect, glm::vec2 point) {
                    return point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y &&
                           point.y < rect.y + rect.height;
                };
                const UIRect inspectorOccluder{inspectorX, 0.0f, std::max(0.0f, viewportSize.x - inspectorX),
                                               inspectorBottom};
                if (!gizmoResult.pressConsumed && !gizmoBlocked && m_uiContext.activeId() == kUINoWidget &&
                    m_uiContext.pressedRect(UIRect{0.0f, 0.0f, viewportSize.x, viewportSize.y}) &&
                    !insideRect(UIRect{6.0f, 6.0f, 372.0f, 124.0f}, mouse) && !insideRect(toolbarRect, mouse) &&
                    !insideRect(outlinerRect, mouse) && !insideRect(inspectorOccluder, mouse) &&
                    !insideRect(assetRect, mouse)) {
                    RaycastHit hit;
                    if (pickEntityAt(m_scene, activeCamera->camera, mouse, viewportSize, hit)) {
                        m_outliner.setSelected(hit.entity);
                        ENGINE_LOG_DEBUG("Editor pick: entity #" << entt::to_integral(hit.entity) << " at "
                                                                 << hit.distance << " m");
                    } else {
                        m_outliner.setSelected(entt::null); // empty-space click deselects
                    }
                }
            }

            // Phase 29: asset browser + drop targets. The browser
            // registers its drag sources; the inspector dock is the
            // assignment target (checked first — it wins over the
            // viewport), the viewport spawns dropped meshes at the
            // ground point under the cursor. Double-clicking a mesh
            // file spawns it through the screen centre.
            // Phase 31: the asset browser spawns into the scene,
            // so it is not built in Play mode (no drag sources, no
            // activation; the drop targets below then stay inert).
            const UIAssetBrowser::Result browserResult =
                playing ? UIAssetBrowser::Result{} : m_assetBrowser.build(m_uiContext, assetRect);
            const UIDragPayload dragPayload = m_uiContext.dragPayload();
            const glm::vec2 dragMouse = m_uiContext.mousePosition();
            const auto overRect = [](const UIRect& rect, glm::vec2 point) {
                return point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y &&
                       point.y < rect.y + rect.height;
            };
            const UIRect inspectorDropRect{inspectorX, 0.0f, std::max(0.0f, viewportSize.x - inspectorX),
                                           inspectorBottom};
            const entt::entity dropSelection = m_outliner.selected();
            if (dropSelection != entt::null && registry.valid(dropSelection)) {
                const bool hot = m_uiContext.dropTargetHovered(inspectorDropRect, "AssetTexture") ||
                                 m_uiContext.dropTargetHovered(inspectorDropRect, "AssetMesh") ||
                                 m_uiContext.dropTargetHovered(inspectorDropRect, "AssetShader");
                if (hot) {
                    ui.drawRoundedRect(inspectorDropRect.x, inspectorDropRect.y, inspectorDropRect.width,
                                       inspectorDropRect.height,
                                       UIRectStyle{6.0f, 2.0f, glm::vec4{1.0f, 0.82f, 0.25f, 0.9f}},
                                       glm::vec4{1.0f, 0.82f, 0.25f, 0.10f});
                }
                if (m_uiContext.dropTarget(inspectorDropRect, "AssetTexture") ||
                    m_uiContext.dropTarget(inspectorDropRect, "AssetMesh") ||
                    m_uiContext.dropTarget(inspectorDropRect, "AssetShader")) {
                    applyDroppedAsset(dropSelection, dragPayload);
                }
            }
            if (const CameraComponent* activeCamera = m_scene.findActiveCamera(); activeCamera != nullptr) {
                const bool overDock = overRect(UIRect{6.0f, 6.0f, 372.0f, 124.0f}, dragMouse) ||
                                      overRect(toolbarRect, dragMouse) || overRect(outlinerRect, dragMouse) ||
                                      overRect(inspectorDropRect, dragMouse) || overRect(assetRect, dragMouse);
                if (!overDock && m_uiContext.dropTarget(UIRect{0.0f, 0.0f, viewportSize.x, viewportSize.y},
                                                        "AssetMesh")) {
                    spawnDroppedMesh(dragPayload.data, dragMouse, activeCamera->camera, viewportSize);
                }
                if (browserResult.activated != nullptr && browserResult.activated->kind == AssetEntryKind::Mesh) {
                    spawnDroppedMesh(browserResult.activated->relativePath,
                                     glm::vec2{viewportSize.x * 0.5f, viewportSize.y * 0.5f}, activeCamera->camera,
                                     viewportSize);
                }
            }
            m_uiContext.endFrame();
        }
        return m_renderSystem.render(m_scene);
    };

    // Phase 29: the Asset Browser opens on the project assets
    // folder (copied next to the executable by the build).
    m_assetBrowser.setRootPath("assets");

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


void Application::handleEventImpulse() {
    // Input action -> gameplay effect, end to end through the Phase 17
    // stack: the "Impulse" action (bound to F in the ctor) pops the
    // editor-selected body up and away from the camera. Selection is
    // the unified editor selection (Phase 27/28: outliner clicks and
    // viewport picking both feed it).
    const entt::entity selected = m_outliner.selected();
    if (!m_input.wasActionPressed("Impulse") || selected == entt::null) {
        return;
    }
    auto& registry = m_scene.registry();
    if (!registry.valid(selected)) {
        return;
    }
    RigidBodyComponent* body = registry.try_get<RigidBodyComponent>(selected);
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
    ENGINE_LOG_INFO("Impulse action: launched entity #" << entt::to_integral(selected));
}

void Application::enterPlayMode() {
    // The selection names an entity the runtime is about to own;
    // drop it so no editor widget keeps a handle across the
    // snapshot boundary (Stop rebuilds every handle anyway).
    m_outliner.setSelected(entt::null);
    if (m_playSession.enterPlay(m_scene)) {
        // The C-key physics reset targets the pre-play state:
        // capture spawn states against the live handles now.
        m_physicsSystem.captureSpawnStates(m_scene);
    }
}

void Application::exitPlayMode() {
    if (m_playSession.exitPlay(m_scene)) {
        // The restore rebuilt every entity: keep the selection
        // cleared rather than pointing at a recycled handle.
        m_outliner.setSelected(entt::null);
    }
}

void Application::saveSceneToGbq() {
    const std::filesystem::path path = m_assetBrowser.rootPath() / "quicksave.gbq";
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    if (serializeScene(m_scene, path)) {
        ENGINE_LOG_INFO("Editor: scene saved to '" << path.string() << "' (" << m_scene.entityCount() << " entities)");
    } else {
        ENGINE_LOG_WARNING("Editor: scene save failed ('" << path.string() << "')");
    }
}

void Application::loadSceneFromGbq() {
    const std::filesystem::path path = m_assetBrowser.rootPath() / "quicksave.gbq";
    // The load rebuilds every entity handle: clear the selection
    // up front so the inspector/gizmo never see a recycled handle.
    m_outliner.setSelected(entt::null);
    if (deserializeScene(m_scene, path)) {
        ENGINE_LOG_INFO("Editor: scene loaded from '" << path.string() << "' (" << m_scene.entityCount() << " entities)");
    } else {
        ENGINE_LOG_WARNING("Editor: scene load failed ('" << path.string() << "'); scene unchanged");
    }
}

void Application::exportGame() {
    const std::filesystem::path bundlePath = m_assetBrowser.rootPath() / "export" / "game.gbq";
    // Export the authored in-memory scene (the live registry while
    // editing; the button is inert in Play mode, so the exported
    // bundle never carries mid-simulation state).
    const ExportReport report = ExportProject(m_scene, m_assets, bundlePath);
    if (report.ok) {
        ENGINE_LOG_INFO("Editor: game exported to '" << bundlePath.string() << "' (" << report.bundleBytes
                                                    << " bytes, " << report.packed.size() << " entries: scene + "
                                                    << (report.packed.size() - 1) << " assets)");
    } else {
        ENGINE_LOG_WARNING("Editor: game export FAILED: " << report.error << " -- " << report.missing.size()
                                                          << " unregistered reference(s):");
        for (const std::string& name : report.missing) {
            ENGINE_LOG_WARNING("Editor:   missing asset '" << name << "'");
        }
    }
}

void Application::spawnDroppedMesh(std::string_view relativePath, glm::vec2 mousePixels, const Camera& camera,
                                   glm::vec2 viewportSize) {
    std::filesystem::path absolute;
    if (!UIAssetBrowser::resolveWithinRoot(m_assetBrowser.rootPath(), relativePath, absolute)) {
        ENGINE_LOG_WARNING("Asset drop: '" << relativePath << "' escapes the asset root; refused");
        return;
    }
    if (!UIAssetBrowser::isImportableMesh(relativePath)) {
        ENGINE_LOG_WARNING("Asset drop: '" << relativePath << "' is recognized, but only OBJ meshes import today");
        return;
    }
    const std::string name = std::filesystem::path{relativePath}.stem().string();
    const MeshAsset* asset = m_assets.mesh(name);
    if (asset == nullptr) {
        if (m_assets.loadObj(name, absolute) == kInvalidAssetId) {
            ENGINE_LOG_WARNING("Asset drop: failed to import '" << relativePath << "'");
            return;
        }
        asset = m_assets.mesh(name);
    }
    if (asset == nullptr) {
        return;
    }
    glm::vec3 position;
    if (!dropPointOnGround(camera, mousePixels, viewportSize, position)) {
        position = camera.position() + camera.front() * 6.0f; // hover it in front of the camera
    }
    const Entity spawned = m_scene.spawnImportedModel(*asset, name, position, 1.0f, 1.0f);
    if (spawned.isValid()) {
        m_outliner.setSelected(spawned.handle());
        ENGINE_LOG_INFO("Asset drop: spawned '" << name << "' at (" << position.x << ", " << position.y << ", "
                                                << position.z << ")");
    }
}

std::string Application::deriveMaterial(MeshComponent& mesh, const std::string& derivedName,
                                        const std::function<void(MaterialDesc&)>& mutate) {
    if (m_assets.material(derivedName) == nullptr) {
        MaterialDesc desc;
        if (const MaterialAsset* base = m_assets.material(mesh.materialName); base != nullptr) {
            desc = base->desc;
        }
        desc.name = derivedName;
        mutate(desc);
        m_assets.registerMaterial(std::move(desc));
    }
    mesh.materialName = derivedName;
    return derivedName;
}

void Application::applyDroppedAsset(entt::entity entity, const UIDragPayload& payload) {
    auto& registry = m_scene.registry();
    if (entity == entt::null || !registry.valid(entity)) {
        return;
    }
    std::filesystem::path absolute;
    if (!UIAssetBrowser::resolveWithinRoot(m_assetBrowser.rootPath(), payload.data, absolute)) {
        ENGINE_LOG_WARNING("Asset drop: '" << payload.data << "' escapes the asset root; refused");
        return;
    }
    const std::string stem = std::filesystem::path{payload.data}.stem().string();
    if (payload.type == "AssetMesh") {
        if (!UIAssetBrowser::isImportableMesh(payload.data)) {
            ENGINE_LOG_WARNING("Asset drop: '" << payload.data << "' is recognized, but only OBJ meshes import");
            return;
        }
        const MeshAsset* asset = m_assets.mesh(stem);
        if (asset == nullptr) {
            if (m_assets.loadObj(stem, absolute) == kInvalidAssetId) {
                ENGINE_LOG_WARNING("Asset drop: failed to import '" << payload.data << "'");
                return;
            }
            asset = m_assets.mesh(stem);
        }
        if (asset == nullptr) {
            return;
        }
        MeshComponent* mesh = registry.try_get<MeshComponent>(entity);
        if (mesh == nullptr) {
            mesh = &registry.emplace<MeshComponent>(entity);
        }
        mesh->assetId = stem;
        mesh->localBounds = asset->bounds.aabb();
        ENGINE_LOG_INFO("Asset drop: entity #" << entt::to_integral(entity) << " now renders '" << stem << "'");
    } else if (payload.type == "AssetTexture") {
        MeshComponent* mesh = registry.try_get<MeshComponent>(entity);
        if (mesh == nullptr) {
            ENGINE_LOG_WARNING("Asset drop: selected entity has no mesh to receive a texture");
            return;
        }
        m_assets.declareTextureSource(stem, absolute);
        m_assets.requestLoad(stem);
        deriveMaterial(*mesh, "drop_" + stem,
                       [&stem](MaterialDesc& desc) { desc.pbr.albedoTexture = stem; });
        ENGINE_LOG_INFO("Asset drop: texture '" << stem << "' assigned as albedo of '" << mesh->materialName << "'");
    } else if (payload.type == "AssetShader") {
        MeshComponent* mesh = registry.try_get<MeshComponent>(entity);
        if (mesh == nullptr) {
            ENGINE_LOG_WARNING("Asset drop: selected entity has no mesh to receive a shader");
            return;
        }
        const std::string fileName = std::filesystem::path{payload.data}.filename().string();
        const bool fragment = fileName.ends_with(".frag.spv");
        const bool vertex = fileName.ends_with(".vert.spv");
        if (!fragment && !vertex) {
            ENGINE_LOG_WARNING("Asset drop: shader '" << fileName << "' is not a .vert.spv/.frag.spv blob");
            return;
        }
        m_assets.loadShader(fileName, absolute);
        deriveMaterial(*mesh, "drop_" + stem, [&fileName, fragment](MaterialDesc& desc) {
            if (fragment) {
                desc.fragmentShader = fileName;
            } else {
                desc.vertexShader = fileName;
            }
        });
        ENGINE_LOG_INFO("Asset drop: shader '" << fileName << "' assigned via '" << mesh->materialName << "'");
    }
    // Anything else (scripts, ...) has no inspector meaning today:
    // the drop target simply never accepts those payload types.
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
