#include "Core/Application.h"

#include "Physics/PhysicsEvents.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
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
#include "Scripting/StalkerAISystem.h"
#include "Scripting/DSLParser.h"
#include "Scripting/DSLComponents.h"
#include "Scene/Systems/PlayerSystem.h"
#include "Scene/Systems/RotationSystem.h"

#include <utility>

namespace engine {

Application::Application(EngineConfig config)
    : m_config(std::move(config)),
      // The editor ALWAYS starts with a free, visible cursor, even
      // if a game-flavoured config asks for capture-on-start: its
      // own panels are the primary surface, and a hidden locked
      // cursor made the whole UI feel broken. Capture is a
      // Play-mode action (click inside the viewport), never a
      // startup state. The standalone player honours the config.
      m_window(m_config.window.width, m_config.window.height, m_config.window.title.c_str(),
               m_config.window.resizable, false),
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
    if (const DynamicFont& font = m_renderer.uiFont(); font.trueType()) {
        ENGINE_LOG_INFO("Editor UI font: '" << font.fontName()
                                           << "' (TrueType, dynamic exact-size glyph cache)");
    } else {
        ENGINE_LOG_WARNING("Editor UI font: no TrueType font found; using the built-in 5x7 fallback font");
    }
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
    // DSL scripts consume the same physics events natively.
    ScriptSystem::attach(m_scriptContext, m_eventBus);
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
    // The editor never captures the cursor at startup (see the
    // Window construction above): Input adopts the free-cursor
    // state, and capture only ever happens in Play mode after an
    // explicit click inside the 3D viewport.
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
    ENGINE_LOG_INFO("Controls: the editor starts in Edit mode with a free cursor -- left-click "
                    "selects in the viewport, hold right-mouse in the viewport to look around, "
                    "1/2/3 switch gizmo tools, Ctrl+S/Ctrl+O save/load. Play (toolbar or Ctrl+P) "
                    "runs the simulation; in Play, click inside the viewport to capture the mouse "
                    "for player look and Esc releases it back to the UI. Stop restores the "
                    "pre-play snapshot.");

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
            // Backrooms DSL scripts tick beside the native ones:
            // input edges/holds are latched from the real Input,
            // physics events arrive via the bus subscription, and
            // the native stalker AI steers whatever the scripts
            // tagged with `behavior = stalker`.
            ScriptSystem::update(m_scene, m_scriptContext, m_input, fixedDeltaSeconds);
            StalkerAISystem::update(m_scene, static_cast<float>(fixedDeltaSeconds));
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
        m_lastUpdateDeltaSeconds = deltaSeconds;
        // Status-bar notifications are transient: they decay here
        // (real time) and the status bar falls back to its mode hint
        // once the timer runs out.
        if (m_statusSecondsRemaining > 0.0f) {
            m_statusSecondsRemaining -= static_cast<float>(deltaSeconds);
        }
        // Asset sync boundary (Phase 20): publish a bounded number of
        // finished async loads per frame, and poll source files for
        // hot reloads once a second.
        m_assets.processCompletedLoads(8);
        m_secondsSinceAssetPoll += deltaSeconds;
        if (m_secondsSinceAssetPoll >= 1.0) {
            m_secondsSinceAssetPoll = 0.0;
            m_assets.pollFileChanges();
        }
        // Game module + native scripts are *game* logic: they tick
        // only in Play mode. (They used to run unconditionally,
        // which let game-test behaviour leak into the editor's Edit
        // mode and mutate the authored scene.) The hot-reload poll
        // stays live in both modes so a rebuilt module is ready for
        // the next Play press.
        m_editorLayout = currentEditorLayout();
        m_moduleElapsedSeconds += deltaSeconds;
        ++m_moduleFrameIndex;
        {
            EngineContext context = buildModuleContext(static_cast<float>(deltaSeconds), 0.0f);
            if (m_playSession.isPlaying()) {
                m_moduleManager.update(context, static_cast<float>(deltaSeconds));
                NativeScriptSystem::update(context, static_cast<float>(deltaSeconds));
            }
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
            handleEventImpulse();
            // Player camera: mouse look only while captured; the
            // physical player is driven by the fixed update.
            CameraSystem::update(m_scene, m_input, deltaSeconds);
        } else {
            // Editor camera: free-cursor look while the right mouse
            // button is held inside the viewport. No capture, ever.
            updateEditorCamera(deltaSeconds);
        }

        if (m_config.loop.statsLogIntervalSeconds > 0.0) {
            m_secondsSinceStatsLog += deltaSeconds;
            if (m_secondsSinceStatsLog >= m_config.loop.statsLogIntervalSeconds) {
                m_secondsSinceStatsLog = 0.0;
                logLoopStats("Loop stats");
            }
        }

        // Toolbar stats line: refreshed a few times per second and
        // drawn with the TrueType UI font in the render callback.
        // (The legacy bitmap HudRenderer is no longer fed at all --
        // its blocky all-caps glyphs were the unreadable overlay in
        // the top-left corner.)
        if (m_config.render.hudEnabled) {
            m_secondsSinceHudUpdate += deltaSeconds;
            if (m_secondsSinceHudUpdate >= m_config.render.hudUpdateIntervalSeconds) {
                m_secondsSinceHudUpdate = 0.0;
                const auto& time = m_gameLoop.time();
                m_statsText = formatHudText(
                    time.framesPerSecond(), time.deltaSeconds() * 1000.0, m_jobSystem.workerCount(),
                    m_vramBudget.usedBytes() / (1024 * 1024), m_config.render.vramBudgetMiB,
                    m_renderSystem.lastDrawnCount(), m_renderSystem.lastCulledCount());
            }
        } else {
            m_statsText.clear();
        }
    };
    // Rendering always runs last, over fully updated scene state. The
    // return value lets GameLoop count only presented frames for FPS.
    callbacks.render = [this](double /*interpolationAlpha*/) {
        // Editor UI: one docked layout per frame, computed from the
        // live framebuffer size (resize / DPI moves reflow it
        // immediately). The 3D scene is confined to
        // `layout.viewport` -- see EditorLayout and
        // Renderer::setSceneViewport. The batch is per-frame
        // (flushed and cleared by the renderer's overlay record).
        {
            UIBatch& ui = m_renderer.uiBatch();
            const uint32_t framebufferWidth = static_cast<uint32_t>(std::max(m_window.framebufferWidth(), 0));
            const uint32_t framebufferHeight = static_cast<uint32_t>(std::max(m_window.framebufferHeight(), 0));
            ui.begin(framebufferWidth, framebufferHeight);
            const bool playingNow = m_playSession.isPlaying();
            const EditorLayout layout = computeEditorLayout(static_cast<float>(framebufferWidth),
                                                            static_cast<float>(framebufferHeight), playingNow,
                                                            m_dockSizes);
            m_editorLayout = layout;
            // Dock splitters read the same layout the panels are
            // about to consume; dragging rewrites m_dockSizes, so
            // next frame's layout (and scene viewport) follows.
            if (!playingNow) {
                updateSplitters(layout);
            } else {
                m_activeSplitter = 0;
                m_hoveredSplitter = 0;
            }
            // Publish the viewport before RenderSystem runs (below,
            // after this block): projection aspect and the Vulkan
            // viewport/scissor both follow this same rectangle.
            m_renderSystem.setSceneViewport(SceneViewportRect{
                static_cast<int32_t>(layout.viewport.x), static_cast<int32_t>(layout.viewport.y),
                static_cast<uint32_t>(std::max(layout.viewport.width, 0.0f)),
                static_cast<uint32_t>(std::max(layout.viewport.height, 0.0f))});

            // Interactive widgets. While the mouse is captured for
            // Play-mode look the pointer is parked off-screen, so
            // the UI stays visible but inert (Esc releases first).
            UIInput uiInput;
            if (!m_input.isMouseCaptured()) {
                const glm::vec2 mouse = uiMousePosition();
                uiInput.mouseX = mouse.x;
                uiInput.mouseY = mouse.y;
                uiInput.mouseDown = m_input.isMouseButtonDown(MouseButton::Left);
                uiInput.keyStep = (m_input.wasKeyPressed(Key::Right) ? 1 : 0) -
                                  (m_input.wasKeyPressed(Key::Left) ? 1 : 0);
                // Right-click context menus + Esc dismissal.
                uiInput.mouseRightDown = m_input.isMouseButtonDown(MouseButton::Right);
                uiInput.escapePressed = m_input.wasKeyPressed(Key::Escape);
                uiInput.scrollY = m_input.scrollDelta().y;
                // Shift turns slider drags/steps fine-grained; the
                // char-callback text feeds the rename textField.
                uiInput.fineModifier =
                    m_input.isKeyDown(Key::LeftShift) || m_input.isKeyDown(Key::RightShift);
                uiInput.textInput = m_input.textInput();
                uiInput.backspacePressed = m_input.wasKeyPressed(Key::Backspace);
                uiInput.enterPressed = m_input.wasKeyPressed(Key::Enter);
                // Script-editor caret keys: edges drive actions,
                // levels drive key repeat. Ctrl+S is Save & Compile.
                uiInput.ctrlModifier =
                    m_input.isKeyDown(Key::LeftControl) || m_input.isKeyDown(Key::RightControl);
                UIInput::EditorKeys& editorKeys = uiInput.editorKeys;
                editorKeys.left = m_input.wasKeyPressed(Key::Left);
                editorKeys.right = m_input.wasKeyPressed(Key::Right);
                editorKeys.up = m_input.wasKeyPressed(Key::Up);
                editorKeys.down = m_input.wasKeyPressed(Key::Down);
                editorKeys.home = m_input.wasKeyPressed(Key::Home);
                editorKeys.end = m_input.wasKeyPressed(Key::End);
                editorKeys.del = m_input.wasKeyPressed(Key::Delete);
                editorKeys.pageUp = m_input.wasKeyPressed(Key::PageUp);
                editorKeys.pageDown = m_input.wasKeyPressed(Key::PageDown);
                editorKeys.tab = m_input.wasKeyPressed(Key::Tab);
                editorKeys.saveCombo = uiInput.ctrlModifier && m_input.wasKeyPressed(Key::S);
                uiInput.editorHeld.left = m_input.isKeyDown(Key::Left);
                uiInput.editorHeld.right = m_input.isKeyDown(Key::Right);
                uiInput.editorHeld.up = m_input.isKeyDown(Key::Up);
                uiInput.editorHeld.down = m_input.isKeyDown(Key::Down);
                uiInput.editorHeld.backspace = m_input.isKeyDown(Key::Backspace);
                if (m_scriptEditor.focused()) {
                    uiInput.keyStep = 0; // arrows belong to the caret, not sliders
                }
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

            // Rename session bookkeeping: the session dies with its
            // target, with Play mode, or on Esc (the textField also
            // restores its own snapshot; the buffer is dropped here).
            if (m_renameTarget != entt::null &&
                (playing || !m_scene.registry().valid(m_renameTarget) ||
                 m_outliner.selected() != m_renameTarget || m_input.wasKeyPressed(Key::Escape))) {
                m_renameTarget = entt::null;
                m_renameBuffer.clear();
                m_renameFocusPending = false;
            }
            // While renaming, typed characters are text, not editor
            // shortcuts: tool keys, scene shortcuts and the update
            // handlers (C/F) all stay inert until commit/cancel.
            const bool renaming = m_renameTarget != entt::null;

            // Phase 28 gizmo tool switching (also on the toolbar below).
            if (!playing && !renaming && m_input.wasKeyPressed(Key::Num1)) {
                m_gizmo.setMode(GizmoMode::Translate);
            } else if (!playing && !renaming && m_input.wasKeyPressed(Key::Num2)) {
                m_gizmo.setMode(GizmoMode::Rotate);
            } else if (!playing && !renaming && m_input.wasKeyPressed(Key::Num3)) {
                m_gizmo.setMode(GizmoMode::Scale);
            }

            // Phase 30 scene file shortcuts: Ctrl+S saves the whole
            // registry to the .gbq quicksave, Ctrl+O restores it
            // (Edit mode only — never serialize the live runtime
            // scene over the authored quicksave). Phase 31: Ctrl+P
            // toggles Play/Stop.
            const bool ctrlHeld =
                m_input.isKeyDown(Key::LeftControl) || m_input.isKeyDown(Key::RightControl);
            if (!renaming && ctrlHeld && m_input.wasKeyPressed(Key::P)) {
                if (playing) {
                    exitPlayMode();
                } else {
                    enterPlayMode();
                }
            } else if (!playing && !renaming && ctrlHeld && m_input.wasKeyPressed(Key::S)) {
                saveSceneToGbq();
            } else if (!playing && !renaming && ctrlHeld && m_input.wasKeyPressed(Key::O)) {
                loadSceneFromGbq();
            }

            // --- Docked chrome --------------------------------------
            // Solid toolbar and status bars spanning the window, and
            // a framed viewport panel between the docks. These are
            // opaque editor surfaces, not overlays floating on the
            // world: the scene pass never rasterizes beneath them.
            const float scale = layout.uiScale;
            const UITheme& theme = m_uiContext.theme(); // charcoal docks, blue/orange accents
            ui.drawRect(layout.toolbar.x, layout.toolbar.y, layout.toolbar.width, layout.toolbar.height,
                        theme.toolbarFill);
            ui.drawRect(layout.toolbar.x, layout.toolbar.y + layout.toolbar.height - 2.0f,
                        layout.toolbar.width, 2.0f,
                        glm::vec4{theme.sliderFill.r, theme.sliderFill.g, theme.sliderFill.b, 0.9f});
            ui.drawRect(layout.statusBar.x, layout.statusBar.y, layout.statusBar.width,
                        layout.statusBar.height, theme.toolbarFill);
            ui.drawRect(layout.statusBar.x, layout.statusBar.y, layout.statusBar.width, 1.0f,
                        theme.panelBorder);
            // No frame around the viewport: the docks tile flush
            // (EditorLayout), so the scene surface runs edge-to-edge
            // against its neighbours and only this small label chip
            // rides on top of it (the scene itself is already
            // clipped to the rectangle by the renderer).
            {
                const std::string viewportLabel = playing ? "Viewport - Play" : "Viewport - Scene";
                const float chipWidth = ui.measureText(viewportLabel, 1.2f) + 20.0f;
                ui.drawRoundedRect(layout.viewport.x + 8.0f, layout.viewport.y + 8.0f, chipWidth, 24.0f,
                                   UIRectStyle{5.0f, 1.0f, theme.panelBorder},
                                   glm::vec4{theme.backgroundFill.r, theme.backgroundFill.g,
                                             theme.backgroundFill.b, 0.92f});
                ui.drawText(viewportLabel, layout.viewport.x + 18.0f, layout.viewport.y + 14.0f, 1.2f,
                            glm::vec4{0.90f, 0.90f, 0.90f, 1.0f});
            }
            // A fresh action notification (Save/Load/Export, context
            // actions) owns the status bar until its timer decays;
            // otherwise the bar carries the mode hint.
            const bool showNotification = m_statusSecondsRemaining > 0.0f && !m_statusMessage.empty();
            const std::string statusText =
                showNotification
                    ? m_statusMessage
                    : (playing
                           ? (m_input.isMouseCaptured()
                                  ? "Play mode - simulation running - Esc releases the mouse - Stop restores the edit scene"
                                  : "Play mode - click inside the viewport to capture the mouse - Stop (or Ctrl+P) restores the edit scene")
                           : "Edit mode - click in the viewport to select - hold right-mouse in the viewport to fly (cursor captured) - Ctrl+P plays the scene");
            ui.drawText(statusText, layout.statusBar.x + 10.0f,
                        layout.statusBar.y + 0.5f * layout.statusBar.height - 6.0f * scale, 1.2f,
                        showNotification ? glm::vec4{0.55f, 0.90f, 0.60f, 1.0f}
                                         : glm::vec4{0.80f, 0.82f, 0.85f, 0.95f});
            if (!m_statsText.empty()) {
                const float statsWidth = ui.measureText(m_statsText, 1.2f);
                const float statsX = std::max(layout.toolbar.x + 8.0f,
                                              layout.toolbar.x + layout.toolbar.width - statsWidth - 12.0f);
                // Stats sit at the toolbar's right edge; on narrow
                // windows they yield (buttons keep their space).
                if (statsX > layout.toolbar.x + layout.toolbar.width * 0.55f) {
                    ui.drawText(m_statsText, statsX,
                                layout.toolbar.y + 0.5f * layout.toolbar.height - 6.0f * scale, 1.2f,
                                glm::vec4{0.92f, 0.95f, 1.0f, 1.0f});
                }
            }

            // Toolbar buttons, laid out left-to-right inside the
            // toolbar rect (never at absolute window coordinates, so
            // they can never drift over the viewport or off-window
            // when it shrinks). The tool switch keeps its accent
            // underline; Play/Stop keep theirs per live mode. In
            // Play mode the authoring buttons still draw but their
            // actions are ignored -- Stop stays live.
            const float buttonY = layout.toolbar.y + 0.5f * (layout.toolbar.height - 30.0f);
            const float buttonH = 30.0f;
            float buttonX = layout.toolbar.x + 10.0f;
            const auto toolbarButton = [&](const char* label, float width) {
                const UIRect rect{buttonX, buttonY, width, buttonH};
                buttonX += width + 6.0f;
                return m_uiContext.button(rect, label);
            };
            ui.drawText("3D Engine", buttonX, buttonY + 4.0f, 1.35f, glm::vec4{0.95f, 0.97f, 1.0f, 1.0f});
            buttonX += ui.measureText("3D Engine", 1.35f) + 18.0f;
            const float toolStartX = buttonX;
            const int activeTool =
                m_gizmo.mode() == GizmoMode::Translate ? 0 : m_gizmo.mode() == GizmoMode::Rotate ? 1 : 2;
            if (toolbarButton("Move", 74.0f) && !playing) {
                m_gizmo.setMode(GizmoMode::Translate);
            }
            if (toolbarButton("Rotate", 74.0f) && !playing) {
                m_gizmo.setMode(GizmoMode::Rotate);
            }
            if (toolbarButton("Scale", 74.0f) && !playing) {
                m_gizmo.setMode(GizmoMode::Scale);
            }
            ui.drawRect(toolStartX + static_cast<float>(activeTool) * 80.0f,
                        buttonY + buttonH + 3.0f, 74.0f, 3.0f, glm::vec4{1.0f, 0.60f, 0.10f, 0.95f});
            buttonX += 10.0f;
            // Scene file commands: Save serializes the registry to
            // the .gbq quicksave, Load restores it wholesale.
            if (toolbarButton("Save", 74.0f) && !playing) {
                saveSceneToGbq();
            }
            if (toolbarButton("Load", 74.0f) && !playing) {
                loadSceneFromGbq();
            }
            buttonX += 10.0f;
            // Play snapshots the scene and starts the simulation;
            // Stop tears it down and restores the snapshot.
            const float playStartX = buttonX;
            if (toolbarButton("Play", 74.0f) && !playing) {
                enterPlayMode();
            }
            if (toolbarButton("Stop", 74.0f) && playing) {
                exitPlayMode();
            }
            ui.drawRect(playStartX + (playing ? 80.0f : 0.0f), buttonY + buttonH + 3.0f, 74.0f, 3.0f,
                        playing ? glm::vec4{1.0f, 0.35f, 0.25f, 0.95f}
                                : glm::vec4{0.35f, 1.0f, 0.55f, 0.95f});
            buttonX += 10.0f;
            // Export packages the Edit-mode scene into the GBQB
            // bundle the standalone player boots. Inert in Play.
            if (toolbarButton("Export", 74.0f) && !playing) {
                exportGame();
            }

            // Docks: Inspector (right), Hierarchy (left), Asset
            // Browser (bottom-centre) -- all solid panels from the
            // one EditorLayout, all Edit-mode only. In Play mode
            // none of them are built (their widgets edit component
            // memory the runtime owns) and the viewport expands.
            auto& registry = m_scene.registry();
            UIInspectorOptions inspectorOptions;
            inspectorOptions.floatMin = 0.0f;
            inspectorOptions.floatMax = 5000.0f;
            const UIRect& inspectorRect = layout.inspector;
            const float inspectorX = inspectorRect.x + 6.0f;
            const float inspectorWidth = std::max(0.0f, inspectorRect.width - 12.0f);
            const entt::entity selected = m_outliner.selected();
            if (selected != m_inspectorSelection) {
                m_inspectorSelection = selected;
                m_inspectorScroll = 0.0f; // a new entity starts at the top
            }
            if (!playing) {
                // One opaque dock behind the per-component panels.
                // The component panels nest INSIDE the dock panel,
                // so the dock's scissor is the clipping authority:
                // overflowing component content now clips cleanly
                // at the dock's edge instead of spilling text over
                // the status bar (each panel used to scissor only
                // to its own rect, wherever that landed).
                m_uiContext.beginPanel("InspectorDock", inspectorRect,
                                       UIRectStyle{-1.0f, -1.0f, glm::vec4{}});
                // Mouse-wheel scrolling: the wheel over the dock
                // shifts the whole panel stack; the dock's nested
                // scissor clips whatever slides past its edges, and
                // hit-testing respects that scissor, so off-screen
                // rows are inert. The clamp uses last frame's
                // measured content height (immediate-mode loop).
                if (m_uiContext.hoveredRect(inspectorRect)) {
                    m_inspectorScroll -=
                        m_uiContext.scrollDelta() * 3.0f * (UIContext::kRowHeight + UIContext::kRowSpacing);
                }
                m_inspectorScroll = UIContext::clampScrollOffset(m_inspectorScroll, m_inspectorContentHeight,
                                                                 inspectorRect.height - 12.0f);
                if (selected != entt::null && registry.valid(selected)) {
                    const float contentTop = inspectorRect.y + 6.0f;
                    float y = contentTop - m_inspectorScroll;
                    if (renaming && m_renameTarget == selected) {
                        // Rename field, armed by either context menu:
                        // Enter writes the TagComponent, Esc abandons.
                        const float renameHeight = 2.0f * UIContext::kPanelPadding +
                                                   2.0f * (UIContext::kRowHeight + UIContext::kRowSpacing);
                        m_uiContext.beginPanel("RenameEntity", UIRect{inspectorX, y, inspectorWidth, renameHeight},
                                               UIRectStyle{8.0f, 1.0f, glm::vec4{}});
                        m_uiContext.label("Rename entity (Enter confirms, Esc cancels)");
                        const bool committed =
                            m_uiContext.textField("Name", &m_renameBuffer, m_renameFocusPending);
                        m_renameFocusPending = false;
                        m_uiContext.endPanel();
                        y += renameHeight + 6.0f;
                        if (committed) {
                            if (!m_renameBuffer.empty()) {
                                // A viewport-picked entity may carry
                                // no tag yet: give it one so the new
                                // name has somewhere to live.
                                if (TagComponent* tag = registry.try_get<TagComponent>(m_renameTarget);
                                    tag != nullptr) {
                                    tag->tag = m_renameBuffer;
                                } else {
                                    registry.emplace<TagComponent>(m_renameTarget, m_renameBuffer);
                                }
                                setStatusMessage("Renamed entity to '" + m_renameBuffer + "'");
                            }
                            m_renameTarget = entt::null;
                            m_renameBuffer.clear();
                        }
                    }
                    y = inspectComponentAt<TransformComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                               inspectorWidth, inspectorOptions);
                    y = inspectComponentAt<RigidBodyComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                               inspectorWidth, inspectorOptions);
                    y = inspectComponentAt<SpinComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                          inspectorWidth, inspectorOptions);
                    y = inspectComponentAt<PointLightComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                                inspectorWidth, inspectorOptions);
                    y = inspectComponentAt<SpotLightComponent>(m_uiContext, registry, selected, inspectorX, y,
                                                               inspectorWidth, inspectorOptions);
                    y = inspectComponentAt<DirectionalLightComponent>(m_uiContext, registry, selected,
                                                                      inspectorX, y, inspectorWidth,
                                                                      inspectorOptions);
                    y = inspectScriptAt(inspectorX, y, inspectorWidth, selected);
                    if (registry.all_of<MeshComponent>(selected)) {
                        y = inspectMaterialAt(m_uiContext, m_assets, registry.get<MeshComponent>(selected),
                                              inspectorX, y, inspectorWidth);
                    }
                    // Total stack height (undo the scroll shift) for
                    // next frame's clamp.
                    m_inspectorContentHeight = (y + m_inspectorScroll) - contentTop;
                } else {
                    bool drewFallback = false;
                    const auto tags = registry.view<TagComponent>();
                    for (const auto entity : tags) {
                        if (tags.get<TagComponent>(entity).tag != "Spacecraft" ||
                            !registry.all_of<RigidBodyComponent>(entity)) {
                            continue;
                        }
                        auto& body = registry.get<RigidBodyComponent>(entity);
                        inspectComponent(m_uiContext,
                                         UIRect{inspectorX, inspectorRect.y + 6.0f, inspectorWidth, 252.0f},
                                         reflect<RigidBodyComponent>(), &body, inspectorOptions);
                        drewFallback = true;
                        break;
                    }
                    if (!drewFallback) {
                        ui.drawText("Inspector", inspectorX + 6.0f, inspectorRect.y + 12.0f, 1.35f,
                                    glm::vec4{0.92f, 0.95f, 1.0f, 1.0f});
                        ui.drawText("Select an entity in the Hierarchy", inspectorX + 6.0f,
                                    inspectorRect.y + 40.0f, 1.2f, glm::vec4{0.62f, 0.68f, 0.78f, 1.0f});
                        ui.drawText("or click one in the viewport.", inspectorX + 6.0f,
                                    inspectorRect.y + 58.0f, 1.2f, glm::vec4{0.62f, 0.68f, 0.78f, 1.0f});
                    }
                }
                m_uiContext.endPanel();
                // Hierarchy context-menu verdicts: the outliner has
                // already executed Duplicate/Create Child/Delete/
                // Reset Transform on the scene; Rename and Focus are
                // host work (text field, camera) finished here.
                const UIOutliner::Result outlinerResult = m_outliner.build(m_uiContext, layout.hierarchy, m_scene);
                switch (outlinerResult.action) {
                    case UIOutliner::Action::RenameRequested:
                        beginRename(outlinerResult.entity);
                        break;
                    case UIOutliner::Action::FocusRequested:
                        focusEditorCameraOn(outlinerResult.entity);
                        setStatusMessage("Camera focused on the selected entity");
                        break;
                    case UIOutliner::Action::TransformReset:
                        setStatusMessage("Transform reset");
                        break;
                    case UIOutliner::Action::Duplicated:
                        setStatusMessage("Entity duplicated");
                        break;
                    case UIOutliner::Action::CreatedChild:
                        setStatusMessage("Child entity created");
                        break;
                    case UIOutliner::Action::Deleted:
                        setStatusMessage("Entity deleted");
                        break;
                    case UIOutliner::Action::None:
                        break;
                }
            }
            const UIRect assetRect = layout.assetBrowser;

            // Selection outline: the selected entity's world bounds
            // projected as an amber wire box in the viewport, so the
            // current pick is unmistakable in 3D (drawn first, the
            // gizmo rides on top). Edit mode only -- in Play the
            // runtime owns the screen. The deselect half of the
            // contract lives in the picking below: a click on empty
            // viewport space clears the selection (and this box).
            if (!playing) {
                if (const CameraComponent* activeCamera = m_scene.findActiveCamera(); activeCamera != nullptr) {
                    TransformGizmo::drawSelectionOutline(ui, m_scene, m_outliner.selected(),
                                                         activeCamera->camera, layout.viewport);
                }
            }

            // Transform gizmo over the selection, then viewport
            // picking -- both strictly inside the viewport panel.
            // The gizmo runs first so a press that grabs a handle
            // never re-picks; picking then fires only for presses
            // the UI left unconsumed, inside the viewport, with no
            // context menu open at frame start and no widget drag.
            if (const CameraComponent* activeCamera = m_scene.findActiveCamera(); activeCamera != nullptr) {
                const bool gizmoBlocked = playing || popupOpenAtFrameStart || m_input.isMouseCaptured();
                const GizmoUpdateResult gizmoResult =
                    playing ? GizmoUpdateResult{}
                            : m_gizmo.update(m_uiContext, m_scene, m_outliner.selected(), activeCamera->camera,
                                             layout.viewport, gizmoBlocked);
                const glm::vec2 mouse = m_uiContext.mousePosition();
                if (!gizmoResult.pressConsumed && !gizmoBlocked && m_activeSplitter == 0 &&
                    m_uiContext.activeId() == kUINoWidget && m_uiContext.pressedRect(layout.viewport)) {
                    RaycastHit hit;
                    if (pickEntityAt(m_scene, activeCamera->camera, mouse, layout.viewport, hit)) {
                        m_outliner.setSelected(hit.entity);
                        ENGINE_LOG_DEBUG("Editor pick: entity #" << entt::to_integral(hit.entity) << " at "
                                                                 << hit.distance << " m");
                    } else {
                        m_outliner.setSelected(entt::null); // empty-space click deselects
                    }
                }
            }

            // Viewport context menu: a right *click* (press and
            // release within a few pixels, still inside the panel)
            // picks the entity under the cursor and raises the same
            // menu the Hierarchy offers. A right *drag* stays camera
            // look (updateEditorCamera) and raises nothing, and a
            // release over a dock never opens anything.
            // Note: no capture exclusion here. The fly-look captures
            // on the press frame itself, so gating on
            // isMouseCaptured() would swallow the press and the menu
            // could never open; the <6 px test below is what keeps a
            // fly drag (hundreds of virtual pixels) from raising it.
            if (!playing) {
                const glm::vec2 mouse = m_uiContext.mousePosition();
                if (m_uiContext.rightPressedRect(layout.viewport)) {
                    m_rightPressPos = mouse;
                    m_rightPressInViewport = true;
                }
                if (m_rightPressInViewport && m_input.wasMouseButtonReleased(MouseButton::Right)) {
                    m_rightPressInViewport = false;
                    if (layout.viewport.contains(mouse.x, mouse.y) &&
                        glm::distance(mouse, m_rightPressPos) < 6.0f && !m_uiContext.popupOpen()) {
                        if (const CameraComponent* activeCamera = m_scene.findActiveCamera();
                            activeCamera != nullptr) {
                            RaycastHit hit;
                            if (pickEntityAt(m_scene, activeCamera->camera, mouse, layout.viewport, hit)) {
                                m_outliner.setSelected(hit.entity);
                                m_viewportContextTarget = hit.entity;
                                m_uiContext.openPopup("ViewportContextMenu", mouse.x, mouse.y);
                            }
                        }
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
            // Bottom dock tabs: Assets browser and the script
            // editor share the dock; the tab strip is 26 px, the
            // content below it. Play mode builds neither.
            UIAssetBrowser::Result browserResult{};
            if (!playing) {
                const float tabHeight = 26.0f;
                const UIRect tabAssets{assetRect.x + 4.0f, assetRect.y + 2.0f, 92.0f, tabHeight - 4.0f};
                const UIRect tabScript{assetRect.x + 100.0f, assetRect.y + 2.0f, 92.0f, tabHeight - 4.0f};
                if (m_uiContext.button(tabAssets, "Assets")) {
                    m_bottomDockTab = BottomDockTab::Assets;
                }
                if (m_uiContext.button(tabScript, "Script")) {
                    m_bottomDockTab = BottomDockTab::Script;
                }
                const UIRect activeTab = m_bottomDockTab == BottomDockTab::Assets ? tabAssets : tabScript;
                ui.drawRect(activeTab.x, assetRect.y + tabHeight - 2.0f, activeTab.width, 2.0f,
                            m_uiContext.theme().focusBorder);
                const UIRect dockContent{assetRect.x, assetRect.y + tabHeight,
                                         assetRect.width, std::max(0.0f, assetRect.height - tabHeight)};
                if (m_bottomDockTab == BottomDockTab::Assets) {
                    browserResult = m_assetBrowser.build(m_uiContext, dockContent);
                } else {
                    drawScriptDock(dockContent, m_lastUpdateDeltaSeconds);
                }
            }
            const UIDragPayload dragPayload = m_uiContext.dragPayload();
            const glm::vec2 dragMouse = m_uiContext.mousePosition();
            // Drop targets are the actual docks: the inspector dock
            // (checked first -- it wins) assigns, the bounded
            // viewport spawns at the ground point under the cursor.
            // Nothing outside those rects accepts a drop, so a drag
            // released over a panel can never spawn through it.
            const UIRect& inspectorDropRect = layout.inspector;
            const entt::entity dropSelection = m_outliner.selected();
            if (!playing && dropSelection != entt::null && registry.valid(dropSelection)) {
                const bool hot = m_uiContext.dropTargetHovered(inspectorDropRect, "AssetTexture") ||
                                 m_uiContext.dropTargetHovered(inspectorDropRect, "AssetMesh") ||
                                 m_uiContext.dropTargetHovered(inspectorDropRect, "AssetShader");
                if (hot) {
                    ui.drawRoundedRect(inspectorDropRect.x, inspectorDropRect.y, inspectorDropRect.width,
                                       inspectorDropRect.height,
                                       UIRectStyle{6.0f, 2.0f, glm::vec4{1.0f, 0.60f, 0.10f, 0.9f}},
                                       glm::vec4{1.0f, 0.60f, 0.10f, 0.10f});
                }
                if (m_uiContext.dropTarget(inspectorDropRect, "AssetTexture") ||
                    m_uiContext.dropTarget(inspectorDropRect, "AssetMesh") ||
                    m_uiContext.dropTarget(inspectorDropRect, "AssetShader")) {
                    applyDroppedAsset(dropSelection, dragPayload);
                }
            }
            if (const CameraComponent* activeCamera = m_scene.findActiveCamera(); activeCamera != nullptr) {
                if (!playing && m_uiContext.dropTarget(layout.viewport, "AssetMesh")) {
                    spawnDroppedMesh(dragPayload.data, dragMouse, activeCamera->camera, layout.viewport);
                }
                if (browserResult.activated != nullptr && browserResult.activated->kind == AssetEntryKind::Mesh) {
                    spawnDroppedMesh(browserResult.activated->relativePath,
                                     glm::vec2{layout.viewport.x + 0.5f * layout.viewport.width,
                                               layout.viewport.y + 0.5f * layout.viewport.height},
                                     activeCamera->camera, layout.viewport);
                }
            }
            // The viewport context menu renders last, on top of
            // every dock (the Hierarchy menu does the same inside
            // UIOutliner::build). Its edits run through the exact
            // helpers the Hierarchy menu uses.
            if (!playing && m_viewportContextTarget != entt::null &&
                !registry.valid(m_viewportContextTarget)) {
                m_viewportContextTarget = entt::null; // deleted via the Hierarchy meanwhile
            }
            if (!playing && m_uiContext.beginPopup("ViewportContextMenu")) {
                const entt::entity target = m_viewportContextTarget;
                if (m_uiContext.menuItem("Duplicate", target != entt::null)) {
                    const entt::entity copy = UIOutliner::duplicateEntity(m_scene, target);
                    if (copy != entt::null) {
                        m_outliner.setSelected(copy);
                        setStatusMessage("Entity duplicated");
                    }
                }
                if (m_uiContext.menuItem("Delete", target != entt::null)) {
                    m_scene.destroyEntity(Entity{target, &m_scene});
                    m_viewportContextTarget = entt::null;
                    setStatusMessage("Entity deleted");
                }
                if (m_uiContext.menuItem("Rename", target != entt::null)) {
                    beginRename(target);
                }
                if (m_uiContext.menuItem("Reset Transform",
                                         target != entt::null &&
                                             registry.try_get<TransformComponent>(target) != nullptr)) {
                    UIOutliner::resetEntityTransform(m_scene, target);
                    setStatusMessage("Transform reset");
                }
                if (m_uiContext.menuItem("Focus", target != entt::null)) {
                    focusEditorCameraOn(target);
                    setStatusMessage("Camera focused on the selected entity");
                }
                m_uiContext.endPopup();
            }
            // Splitter affordance: accent line on the hovered /
            // dragged dock boundary, and the matching OS cursor.
            if (!playing) {
                const int splitter = m_activeSplitter != 0 ? m_activeSplitter : m_hoveredSplitter;
                const glm::vec4 accent = m_uiContext.theme().focusBorder;
                if (splitter == 1) {
                    ui.drawRect(layout.hierarchy.x + layout.hierarchy.width - 1.0f, layout.hierarchy.y, 2.0f,
                                layout.hierarchy.height, accent);
                } else if (splitter == 2) {
                    ui.drawRect(layout.inspector.x - 1.0f, layout.inspector.y, 2.0f, layout.inspector.height, accent);
                } else if (splitter == 3) {
                    ui.drawRect(layout.assetBrowser.x, layout.assetBrowser.y - 1.0f, layout.assetBrowser.width,
                                2.0f, accent);
                }
                Window::CursorShape desired = Window::CursorShape::Arrow;
                if (splitter == 1 || splitter == 2) {
                    desired = Window::CursorShape::ResizeEastWest;
                } else if (splitter == 3) {
                    desired = Window::CursorShape::ResizeNorthSouth;
                } else if (m_bottomDockTab == BottomDockTab::Script &&
                           m_scriptEditorRect.contains(m_uiContext.mousePosition().x,
                                                       m_uiContext.mousePosition().y)) {
                    desired = Window::CursorShape::IBeam;
                }
                m_window.setCursorShape(desired);
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

// ---------------------------------------------------------------------------
// Script dock + dock splitters
// ---------------------------------------------------------------------------

void Application::bindScriptEditor(entt::entity entity) {
    auto& registry = m_scene.registry();
    if (entity == entt::null || !registry.valid(entity) || !registry.all_of<ScriptComponent>(entity)) {
        return;
    }
    const ScriptComponent& script = registry.get<ScriptComponent>(entity);
    std::string name = "Entity " + std::to_string(entt::to_integral(entity));
    if (const TagComponent* tag = registry.try_get<TagComponent>(entity); tag != nullptr && !tag->tag.empty()) {
        name = tag->tag;
    }
    m_scriptEditor.loadText(script.source);
    m_scriptEditor.bind(entt::to_integral(entity), name, script.filePath);
    m_scriptStatus.clear();
    m_bottomDockTab = BottomDockTab::Script;
}

void Application::saveAndCompileScript() {
    const entt::entity entity = static_cast<entt::entity>(m_scriptEditor.boundEntity());
    auto& registry = m_scene.registry();
    if (m_scriptEditor.boundEntity() == TextEditor::kNoEntity || !registry.valid(entity) ||
        !registry.all_of<ScriptComponent>(entity)) {
        setStatusMessage("Save & Compile: no script is bound in the editor");
        return;
    }
    ScriptComponent& script = registry.get<ScriptComponent>(entity);
    script.source = m_scriptEditor.text();
    script.filePath = m_scriptEditor.filePath();
    bool fileWritten = true;
    if (!script.filePath.empty()) {
        std::ofstream out(script.filePath, std::ios::binary | std::ios::trunc);
        if (out) {
            out << script.source;
            m_scriptContext.fileStamps.erase(entity); // our own write is not an external edit
        } else {
            fileWritten = false;
        }
    }
    // Force a fresh compile on the next ScriptSystem tick, and get
    // the verdict synchronously for the dock/inspector status line.
    ScriptSystem::clearRuntime(m_scriptContext, entity);
    const DSLParseResult parsed = parseBackroomsScript(script.source);
    if (parsed.ok()) {
        m_scriptStatus = "Compiled OK";
        setStatusMessage("Script compiled: " + m_scriptEditor.displayName() +
                         (fileWritten ? "" : " (file write failed)"));
    } else {
        m_scriptStatus = parsed.errors.empty() ? std::string("Compile failed") : parsed.errors.front();
        setStatusMessage("Script error: " + m_scriptStatus);
    }
    m_scriptEditor.markClean();
}

float Application::inspectScriptAt(float x, float y, float width, entt::entity entity) {
    auto& registry = m_scene.registry();
    const float rowStep = UIContext::kRowHeight + UIContext::kRowSpacing;
    if (!registry.all_of<ScriptComponent>(entity)) {
        const float height = 2.0f * UIContext::kPanelPadding + 2.0f * rowStep;
        m_uiContext.beginPanel("ScriptComponent", UIRect{x, y, width, height}, UIRectStyle{8.0f, 1.0f, glm::vec4{}});
        m_uiContext.label("Script (DSL)");
        if (m_uiContext.button("Add Script Component")) {
            ScriptComponent& script = registry.emplace<ScriptComponent>(entity);
            script.source = "from start()\n"
                            "    // Describe the behavior here.\n"
                            "    on_interact(\"item\") {\n"
                            "        print(\"Interacted\")\n"
                            "    }\n"
                            "script(end)\n";
            bindScriptEditor(entity);
            setStatusMessage("Script component added (Script tab, bottom dock)");
        }
        m_uiContext.endPanel();
        return y + height + 6.0f;
    }

    ScriptComponent& script = registry.get<ScriptComponent>(entity);
    const float height = 2.0f * UIContext::kPanelPadding + 8.0f * rowStep;
    m_uiContext.beginPanel("ScriptComponent", UIRect{x, y, width, height}, UIRectStyle{8.0f, 1.0f, glm::vec4{}});
    m_uiContext.label("Script (DSL)");
    m_uiContext.label("File (.dsla, empty = inline)");
    if (m_uiContext.textField("ScriptFilePath", &script.filePath) &&
        static_cast<entt::entity>(m_scriptEditor.boundEntity()) == entity) {
        m_scriptEditor.setFilePath(script.filePath);
    }
    m_uiContext.checkbox("Enabled", &script.enabled);
    bool autoReload = hasFlag(script.compileFlags, ScriptCompileFlags::AutoReload);
    if (m_uiContext.checkbox("Auto-reload file", &autoReload)) {
        script.compileFlags = autoReload ? (script.compileFlags | ScriptCompileFlags::AutoReload)
                                         : static_cast<ScriptCompileFlags>(static_cast<uint32_t>(script.compileFlags) &
                                                                           ~static_cast<uint32_t>(ScriptCompileFlags::AutoReload));
    }
    const ScriptSystemContext::Runtime* runtime = ScriptSystem::findRuntime(m_scriptContext, entity);
    if (!script.enabled) {
        m_uiContext.label("Status: disabled");
    } else if (runtime == nullptr) {
        m_uiContext.label("Status: not compiled yet");
    } else if (!runtime->errors.empty()) {
        std::string status = "Error: " + runtime->errors.front();
        if (status.size() > 52) {
            status.resize(52);
        }
        m_uiContext.label(status);
    } else {
        size_t lines = 1;
        for (const char ch : script.source) {
            if (ch == '\n') {
                ++lines;
            }
        }
        m_uiContext.label("Status: compiled OK (" + std::to_string(lines) + " lines)");
    }
    if (m_uiContext.button("Open in Script Editor")) {
        bindScriptEditor(entity);
    }
    if (m_uiContext.button("Save & Compile")) {
        if (static_cast<entt::entity>(m_scriptEditor.boundEntity()) == entity) {
            saveAndCompileScript();
        } else {
            ScriptSystem::clearRuntime(m_scriptContext, entity);
            const DSLParseResult parsed = parseBackroomsScript(script.source);
            setStatusMessage(parsed.ok() ? "Script compiled OK" : "Script error: " + parsed.errors.front());
        }
    }
    m_uiContext.endPanel();
    return y + height + 6.0f;
}

void Application::drawScriptDock(const UIRect& rect, double deltaSeconds) {
    m_scriptEditorRect = rect;
    auto& registry = m_scene.registry();
    // Follow the selection while the buffer is clean: clicking a
    // scripted entity in the Hierarchy loads its source here. A
    // dirty buffer is never silently replaced.
    const entt::entity selected = m_outliner.selected();
    if (!m_scriptEditor.dirty() && selected != entt::null && registry.valid(selected) &&
        registry.all_of<ScriptComponent>(selected) &&
        static_cast<entt::entity>(m_scriptEditor.boundEntity()) != selected) {
        bindScriptEditor(selected);
    }
    const float headerHeight = 30.0f;
    std::string title = "Script Editor";
    const entt::entity bound = static_cast<entt::entity>(m_scriptEditor.boundEntity());
    const bool boundValid = m_scriptEditor.boundEntity() != TextEditor::kNoEntity && registry.valid(bound) &&
                            registry.all_of<ScriptComponent>(bound);
    if (boundValid) {
        title += ": " + m_scriptEditor.displayName();
        if (!m_scriptEditor.filePath().empty()) {
            title += "  (" + m_scriptEditor.filePath() + ")";
        }
        if (m_scriptEditor.dirty()) {
            title += " *";
        }
        if (!m_scriptStatus.empty()) {
            title += "  -  " + m_scriptStatus;
        }
    }
    m_renderer.uiBatch().drawText(title, rect.x + 8.0f, rect.y + 8.0f, 1.3f, m_uiContext.theme().text);
    const UIRect saveButton{rect.x + rect.width - 150.0f, rect.y + 3.0f, 142.0f, 24.0f};
    if (boundValid && m_uiContext.button(saveButton, "Save & Compile")) {
        saveAndCompileScript();
    }
    const UIRect editorRect{rect.x + 4.0f, rect.y + headerHeight, std::max(0.0f, rect.width - 8.0f),
                            std::max(0.0f, rect.height - headerHeight - 4.0f)};
    m_scriptEditorRect = editorRect;
    if (!boundValid) {
        m_renderer.uiBatch().drawText("No script bound. Select an entity, then Inspector > Add Script Component",
                                      rect.x + 8.0f, rect.y + headerHeight + 10.0f, 1.25f,
                                      glm::vec4{0.62f, 0.68f, 0.78f, 1.0f});
        m_renderer.uiBatch().drawText("(or \"Open in Script Editor\" on an entity that already has one).",
                                      rect.x + 8.0f, rect.y + headerHeight + 30.0f, 1.25f,
                                      glm::vec4{0.62f, 0.68f, 0.78f, 1.0f});
        return;
    }
    const TextEditor::Result result =
        m_scriptEditor.draw(m_uiContext, editorRect, static_cast<float>(deltaSeconds));
    if (result.saveRequested) {
        saveAndCompileScript();
    }
}

void Application::updateSplitters(const EditorLayout& layout) {
    if (m_input.isMouseCaptured()) {
        m_hoveredSplitter = 0;
        m_activeSplitter = 0;
        return;
    }
    const glm::vec2 mouse = uiMousePosition();
    const float halfBand = 3.5f;
    const float middleTop = layout.hierarchy.y;
    const float middleBottom = layout.hierarchy.y + layout.hierarchy.height;
    int hovered = 0;
    if (mouse.y >= middleTop && mouse.y < middleBottom) {
        if (std::abs(mouse.x - (layout.hierarchy.x + layout.hierarchy.width)) <= halfBand) {
            hovered = 1;
        } else if (std::abs(mouse.x - layout.inspector.x) <= halfBand) {
            hovered = 2;
        }
    }
    if (hovered == 0 && std::abs(mouse.y - layout.assetBrowser.y) <= halfBand && mouse.x >= layout.assetBrowser.x &&
        mouse.x < layout.assetBrowser.x + layout.assetBrowser.width) {
        hovered = 3;
    }
    m_hoveredSplitter = hovered;
    if (m_activeSplitter == 0 && hovered != 0 && m_input.wasMouseButtonPressed(MouseButton::Left)) {
        m_activeSplitter = hovered;
    }
    if (m_activeSplitter == 0) {
        return;
    }
    if (!m_input.isMouseButtonDown(MouseButton::Left)) {
        m_activeSplitter = 0;
        return;
    }
    // Sizes are recomputed by computeEditorLayout next frame (with
    // its clamps + minimum-viewport guarantee); the scene viewport
    // published from that layout follows the drag immediately.
    const float framebufferRight = layout.inspector.x + layout.inspector.width;
    const float dockBottom = layout.assetBrowser.y + layout.assetBrowser.height;
    switch (m_activeSplitter) {
    case 1: m_dockSizes.leftWidth = mouse.x - layout.hierarchy.x; break;
    case 2: m_dockSizes.rightWidth = framebufferRight - mouse.x; break;
    case 3: m_dockSizes.bottomHeight = dockBottom - mouse.y; break;
    default: break;
    }
}

EditorLayout Application::currentEditorLayout() const {
    return computeEditorLayout(static_cast<float>(std::max(m_window.framebufferWidth(), 0)),
                               static_cast<float>(std::max(m_window.framebufferHeight(), 0)),
                               m_playSession.isPlaying(), m_dockSizes);
}

glm::vec2 Application::uiMousePosition() const {
    // GLFW reports logical window coordinates; the UI batch and the
    // dock layout live in framebuffer pixels. The two only diverge
    // under DPI/content scaling, where this factor is > 1.
    return m_input.mousePosition() * m_window.uiCoordinateScale();
}

void Application::updateEditorCamera(double deltaSeconds) {
    CameraComponent* activeCamera = m_scene.findActiveCamera();
    if (activeCamera == nullptr) {
        return;
    }
    // Fly navigation lives entirely behind the right mouse button
    // held inside the viewport panel: mouse motion looks around and
    // WASD/QE fly along the camera basis (Shift sprints), smoothed
    // by the Camera's look/velocity convergence below. For the
    // whole drag the cursor is captured (hidden and locked by
    // handleMouseCapture), so the look is driven by raw incremental
    // deltas and never stalls at a window edge; releasing the
    // button hands the free cursor back to the UI. While a rename
    // text field owns the keyboard, the same keys are text, never
    // flight.
    Camera& camera = activeCamera->camera;
    const bool flying = m_editorFlyCapture && m_renameTarget == entt::null;
    if (flying) {
        camera.processLook(m_input.mouseDelta());
        camera.processMovement(m_input, deltaSeconds);
    } else {
        // Keep integrating with a zero desired velocity so a flight
        // in progress glides to a stop over a few frames instead of
        // halting dead the moment the button is released.
        camera.updateMovement(glm::vec3{0.0f}, deltaSeconds);
    }
    camera.update(deltaSeconds);
    // Deliberately no movement and no PlayerSystem::syncCamera here:
    // in Edit mode the authored player transform is scene data, and
    // the editor camera must not rewrite it (or be teleported by it).
}

void Application::handleMouseCapture() {
    if (!m_playSession.isPlaying()) {
        // Edit mode: the fly-look owns the cursor capture for
        // exactly the duration of a right-button drag that began
        // inside the viewport. Engaging capture switches GLFW to
        // GLFW_CURSOR_DISABLED (Window::setCursorCaptured): the
        // pointer hides and locks, so look-around is driven by raw
        // incremental deltas and can never die at a window border
        // mid-drag. Releasing the button restores
        // GLFW_CURSOR_NORMAL and the UI is live again -- there is
        // no captured state outside the drag. While a rename owns
        // the keyboard the right button stays a UI gesture
        // (context menus), never a capture, and Play mode starts
        // from a clean slate (see below).
        const glm::vec2 mouse = uiMousePosition();
        const bool pressInViewport = m_input.wasMouseButtonPressed(MouseButton::Right) &&
                                     m_renameTarget == entt::null &&
                                     m_editorLayout.containsViewport(mouse.x, mouse.y);
        m_editorFlyCapture = (m_editorFlyCapture || pressInViewport) &&
                             m_input.isMouseButtonDown(MouseButton::Right);
        if (m_input.isMouseCaptured() != m_editorFlyCapture) {
            m_input.setMouseCaptured(m_editorFlyCapture);
            if (m_editorFlyCapture) {
                ENGINE_LOG_DEBUG("Mouse captured (viewport fly-look)");
            } else {
                ENGINE_LOG_DEBUG("Mouse released (fly-look ended)");
            }
        }
        return;
    }
    // Entering Play mode: the fly-look capture never carries over.
    if (m_editorFlyCapture) {
        m_editorFlyCapture = false;
        m_input.setMouseCaptured(false);
    }
    // Play mode: Esc always hands the mouse back to the UI first.
    // It never quits the application from here -- the window close
    // button owns quitting, so a stray Esc can't nuke the session.
    if (m_input.wasKeyPressed(Key::Escape)) {
        if (m_input.isMouseCaptured()) {
            m_input.setMouseCaptured(false);
            ENGINE_LOG_DEBUG("Mouse released (Esc)");
        }
        return;
    }
    // Capture only on an explicit click *inside the 3D viewport* --
    // never on toolbar/panel clicks (the old any-click capture is
    // what locked users out of the UI: the first click anywhere,
    // including on a button, hid and locked the cursor).
    if (!m_input.isMouseCaptured() && m_config.input.captureMouseOnClick &&
        m_input.wasMouseButtonPressed(MouseButton::Left)) {
        const glm::vec2 mouse = uiMousePosition();
        if (m_editorLayout.containsViewport(mouse.x, mouse.y)) {
            m_input.setMouseCaptured(true);
            ENGINE_LOG_DEBUG("Mouse captured (viewport click in Play mode)");
        }
    }
}


void Application::handleEventImpulse() {
    // Input action -> gameplay effect, end to end through the Phase 17
    // stack: the "Impulse" action (bound to F in the ctor) pops the
    // editor-selected body up and away from the camera. Selection is
    // the unified editor selection (Phase 27/28: outliner clicks and
    // viewport picking both feed it).
    if (m_renameTarget != entt::null) {
        return; // typing a name: 'f' is text, not the Impulse action
    }
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
    // Play starts with a FREE cursor: the user clicks Stop with the
    // mouse, or clicks inside the viewport to capture it for look.
    if (m_input.isMouseCaptured()) {
        m_input.setMouseCaptured(false);
    }
    if (m_playSession.enterPlay(m_scene)) {
        // The C-key physics reset targets the pre-play state:
        // capture spawn states against the live handles now.
        m_physicsSystem.captureSpawnStates(m_scene);
        // DSL scripts restart from their setup state each Play.
        // (The bus subscriptions capture this member by reference,
        // so resetting its contents is enough -- never re-attach.)
        m_scriptContext = ScriptSystemContext{};
    }
}

void Application::exitPlayMode() {
    // Back to Edit: the cursor belongs to the UI again, always.
    if (m_input.isMouseCaptured()) {
        m_input.setMouseCaptured(false);
    }
    if (m_playSession.exitPlay(m_scene)) {
        // The restore rebuilt every entity: keep the selection
        // cleared rather than pointing at a recycled handle.
        m_outliner.setSelected(entt::null);
    }
}

void Application::saveSceneToGbq() {
    // Save always lands on the active scene path (the editor's
    // .gbq quicksave under the asset root today); the status bar
    // confirms it so the button never feels dead.
    const std::filesystem::path path = m_assetBrowser.rootPath() / "quicksave.gbq";
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    m_activeScenePath = path;
    if (serializeScene(m_scene, path)) {
        ENGINE_LOG_INFO("Editor: scene saved to '" << path.string() << "' (" << m_scene.entityCount() << " entities)");
        setStatusMessage("Saved scene to '" + path.filename().string() + "' (" +
                         std::to_string(m_scene.entityCount()) + " entities)");
    } else {
        ENGINE_LOG_WARNING("Editor: scene save failed ('" << path.string() << "')");
        setStatusMessage("Save failed: could not write '" + path.filename().string() + "'");
    }
}

void Application::loadSceneFromGbq() {
    const std::filesystem::path path = m_assetBrowser.rootPath() / "quicksave.gbq";
    // The load rebuilds every entity handle: clear the selection
    // up front so the inspector/gizmo never see a recycled handle.
    m_outliner.setSelected(entt::null);
    m_activeScenePath = path;
    if (deserializeScene(m_scene, path)) {
        ENGINE_LOG_INFO("Editor: scene loaded from '" << path.string() << "' (" << m_scene.entityCount() << " entities)");
        setStatusMessage("Loaded scene from '" + path.filename().string() + "' (" +
                         std::to_string(m_scene.entityCount()) + " entities)");
    } else {
        ENGINE_LOG_WARNING("Editor: scene load failed ('" << path.string() << "'); scene unchanged");
        setStatusMessage("Load failed: '" + path.filename().string() + "' is missing or unreadable");
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
        setStatusMessage("Exported standalone bundle '" + bundlePath.filename().string() + "' (" +
                         std::to_string(report.bundleBytes) + " bytes, " +
                         std::to_string(report.packed.size()) + " entries)");
    } else {
        ENGINE_LOG_WARNING("Editor: game export FAILED: " << report.error << " -- " << report.missing.size()
                                                          << " unregistered reference(s):");
        for (const std::string& name : report.missing) {
            ENGINE_LOG_WARNING("Editor:   missing asset '" << name << "'");
        }
        setStatusMessage("Export failed: " + report.error + " (" + std::to_string(report.missing.size()) +
                         " unregistered reference(s), see log)");
    }
}

void Application::setStatusMessage(std::string message) {
    m_statusMessage = std::move(message);
    m_statusSecondsRemaining = 4.0f; // decayed in the update callback
}

void Application::beginRename(entt::entity entity) {
    auto& registry = m_scene.registry();
    if (entity == entt::null || !registry.valid(entity)) {
        return;
    }
    m_renameTarget = entity;
    m_renameBuffer.clear();
    if (const TagComponent* tag = registry.try_get<TagComponent>(entity); tag != nullptr) {
        m_renameBuffer = tag->tag;
    }
    m_renameFocusPending = true; // the field grabs the keyboard on its first frame
}

void Application::focusEditorCameraOn(entt::entity entity) {
    CameraComponent* activeCamera = m_scene.findActiveCamera();
    auto& registry = m_scene.registry();
    if (activeCamera == nullptr || entity == entt::null || !registry.valid(entity)) {
        return;
    }
    // Frame the selection box (the exact volume picking tests):
    // centre the camera's current view direction on it, pulled
    // back far enough for the whole box to read. Orientation is
    // untouched — Focus repositions, it never re-aims the editor.
    glm::vec3 target{0.0f};
    float radius = 1.0f;
    std::array<glm::vec3, 8> corners{};
    if (TransformGizmo::selectionBoxCorners(m_scene, entity, corners)) {
        for (const glm::vec3& corner : corners) {
            target += corner;
        }
        target /= static_cast<float>(corners.size());
        radius = 0.0f;
        for (const glm::vec3& corner : corners) {
            radius = std::max(radius, glm::distance(target, corner));
        }
    } else if (const TransformComponent* transform = registry.try_get<TransformComponent>(entity);
               transform != nullptr) {
        target = transform->position;
    } else {
        return;
    }
    Camera& camera = activeCamera->camera;
    const float distance = std::clamp(3.0f * radius, 2.0f, 60.0f);
    camera.setPosition(target - camera.front() * distance);
}

void Application::spawnDroppedMesh(std::string_view relativePath, glm::vec2 mousePixels, const Camera& camera,
                                   const UIRect& viewport) {
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
    if (!dropPointOnGround(camera, mousePixels, viewport, position)) {
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
    if (m_renameTarget != entt::null) {
        return; // typing a name: 'c' is text, not the reset shortcut
    }
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
