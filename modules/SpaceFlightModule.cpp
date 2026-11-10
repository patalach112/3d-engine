// ---------------------------------------------------------------------------
// SpaceFlightModule (Phase 24, refactored in Phase 25) — a real game
// module for the plugin system, shipped as EngineRuntimeCore_<sig>
// and loaded through ModuleManager like any third-party game.
//
// The module itself only builds the sandbox on load (procedural
// asteroid field, ship, nozzle lights — all find-or-create against
// engine-owned scene state). All per-frame behaviour lives in
// SpaceFlightScript, an INativeScript attached to the ship through
// NativeScriptComponent and driven by the host's NativeScriptSystem
// — the Unity/UE component model, not a monolithic module loop.
// Editing FlightParams (SpaceFlightModel.h), rebuilding this
// module and letting the hot-reload poll swap it in retunes the
// flight model mid-flight: the module's onUnload destroys the old
// script instance before the library unloads, and the next script
// tick re-creates it from the new generation's factory, on the same
// ship, with the same velocity.
//
// Flight model: true Newtonian 6-DOF — pilot input becomes
// forces/torques accumulated into the ship's RigidBodyComponent;
// the engine physics integrates them (zero drag, angular momentum
// conserved; the only counter-torque is the RCS flight-assist
// damper, modeled thruster work — see SpaceFlightModel.h).
// ---------------------------------------------------------------------------

#include "Core/GameModule.h"
#include "SpaceFlightModel.h"

#include "Assets/AssetManager.h"
#include "Geometry/Bounds.h"
#include "Geometry/Vertex.h"
#include "Physics/PhysicsComponents.h"
#include "Platform/Input.h"
#include "Scene/Components.h"
#include "Scene/NativeScript.h"
#include "Scene/Reflection.h"
#include "Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace spaceflight;

constexpr const char* kShipTag = "Spacecraft";
constexpr const char* kFlightScriptName = "SpaceFlightScript";

struct NozzleSpec {
    const char* tag;
    glm::vec3 offset;
    glm::vec3 color;
    float range;
    float scale;
    bool main;
};

constexpr std::array<NozzleSpec, 3> kNozzles{{
    {"ThrusterNozzleMain", glm::vec3{0.0f, 0.0f, 1.75f}, glm::vec3{1.0f, 0.52f, 0.18f}, 36.0f, 0.34f, true},
    {"ThrusterNozzlePort", glm::vec3{-1.0f, 0.0f, 0.8f}, glm::vec3{0.5f, 0.72f, 1.0f}, 18.0f, 0.2f, false},
    {"ThrusterNozzleStarboard", glm::vec3{1.0f, 0.0f, 0.8f}, glm::vec3{0.5f, 0.72f, 1.0f}, 18.0f, 0.2f, false},
}};

// Deterministic per-index hash -> [0, 1). Keeps the asteroid field
// identical across loads and reloads without storing any state.
float hash01(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    return static_cast<float>(value >> 8) * (1.0f / 16777216.0f);
}

entt::entity findByTag(engine::Scene& scene, const char* tag) {
    auto view = scene.registry().view<engine::TagComponent>();
    for (const entt::entity entity : view) {
        if (view.get<engine::TagComponent>(entity).tag == tag) {
            return entity;
        }
    }
    return entt::null;
}

size_t countByTag(engine::Scene& scene, const char* tag) {
    size_t count = 0;
    auto view = scene.registry().view<engine::TagComponent>();
    for (const entt::entity entity : view) {
        if (view.get<engine::TagComponent>(entity).tag == tag) {
            ++count;
        }
    }
    return count;
}

// --- Procedural meshes ------------------------------------------------------

// Subdivided icosahedron with per-vertex radial jitter and faceted
// (per-face) normals: a rocky body from 80 triangles.
void buildAsteroidMesh(std::vector<engine::Vertex>& vertices, std::vector<uint32_t>& indices) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<glm::vec3> base{{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (glm::vec3& v : base) {
        v = glm::normalize(v);
    }
    std::vector<std::array<uint32_t, 3>> faces{
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2},
        {10, 7, 6},  {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6},   {3, 6, 8},   {3, 8, 9},   {4, 9, 5},
        {2, 4, 11},  {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
    };
    // One subdivision pass (midpoint cache keeps the shell watertight).
    std::vector<glm::vec3> points = base;
    std::vector<std::pair<uint32_t, uint32_t>> edgeKeys;
    std::vector<uint32_t> edgeMidpoints;
    const auto midpoint = [&points, &edgeKeys, &edgeMidpoints](uint32_t a, uint32_t b) {
        const std::pair<uint32_t, uint32_t> key = std::minmax(a, b);
        for (size_t i = 0; i < edgeKeys.size(); ++i) {
            if (edgeKeys[i] == key) {
                return edgeMidpoints[i];
            }
        }
        const uint32_t index = static_cast<uint32_t>(points.size());
        points.push_back(glm::normalize((points[a] + points[b]) * 0.5f));
        edgeKeys.push_back(key);
        edgeMidpoints.push_back(index);
        return index;
    };
    std::vector<std::array<uint32_t, 3>> subdivided;
    for (const auto& face : faces) {
        const uint32_t ab = midpoint(face[0], face[1]);
        const uint32_t bc = midpoint(face[1], face[2]);
        const uint32_t ca = midpoint(face[2], face[0]);
        subdivided.push_back({face[0], ab, ca});
        subdivided.push_back({face[1], bc, ab});
        subdivided.push_back({face[2], ca, bc});
        subdivided.push_back({ab, bc, ca});
    }
    // Radial jitter (deterministic per point), then flatten to a
    // faceted triangle soup.
    for (size_t i = 0; i < points.size(); ++i) {
        points[i] *= 0.78f + 0.5f * hash01(static_cast<uint32_t>(i) * 2654435761u);
    }
    vertices.reserve(subdivided.size() * 3);
    indices.reserve(subdivided.size() * 3);
    for (size_t f = 0; f < subdivided.size(); ++f) {
        const glm::vec3 a = points[subdivided[f][0]];
        const glm::vec3 b = points[subdivided[f][1]];
        const glm::vec3 c = points[subdivided[f][2]];
        const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
        const float shade = 0.30f + 0.16f * hash01(static_cast<uint32_t>(f) * 40503u + 7u);
        const glm::vec3 color{shade, shade * 0.96f, shade * 0.90f};
        for (const glm::vec3& p : {a, b, c}) {
            indices.push_back(static_cast<uint32_t>(vertices.size()));
            vertices.push_back(engine::Vertex{p, normal, color, glm::vec2{0.0f}});
        }
    }
}

// Elongated octahedron dart: nose -Z, tail +Z, smooth normals.
void buildShipMesh(std::vector<engine::Vertex>& vertices, std::vector<uint32_t>& indices) {
    const std::vector<glm::vec3> points{
        {0.0f, 0.0f, -2.4f},  // 0 nose
        {0.0f, 0.7f, -0.2f},  // 1 top
        {0.85f, 0.0f, -0.2f}, // 2 starboard
        {0.0f, -0.7f, -0.2f}, // 3 bottom
        {-0.85f, 0.0f, -0.2f}, // 4 port
        {0.0f, 0.0f, 1.6f},   // 5 tail
    };
    const std::vector<std::array<uint32_t, 3>> faces{
        {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1}, {5, 2, 1}, {5, 3, 2}, {5, 4, 3}, {5, 1, 4},
    };
    std::vector<glm::vec3> normals(points.size(), glm::vec3{0.0f});
    for (const auto& face : faces) {
        const glm::vec3 n = glm::cross(points[face[1]] - points[face[0]], points[face[2]] - points[face[0]]);
        for (const uint32_t index : face) {
            normals[index] += n;
        }
    }
    vertices.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        vertices.push_back(engine::Vertex{points[i], glm::normalize(normals[i]),
                                          glm::vec3{0.75f, 0.78f, 0.85f}, glm::vec2{0.0f}});
    }
    for (const auto& face : faces) {
        indices.insert(indices.end(), face.begin(), face.end());
    }
}

// --- Sandbox construction (all find-or-create: reload-safe) -----------------

void registerAssets(engine::AssetManager& assets) {
    {
        std::vector<engine::Vertex> vertices;
        std::vector<uint32_t> indices;
        buildAsteroidMesh(vertices, indices);
        assets.registerMesh("asteroid", vertices, indices);
    }
    {
        std::vector<engine::Vertex> vertices;
        std::vector<uint32_t> indices;
        buildShipMesh(vertices, indices);
        assets.registerMesh("spacecraft", vertices, indices);
    }
    engine::MaterialDesc ship;
    ship.name = "spacecraft";
    ship.baseColor = glm::vec4{0.72f, 0.76f, 0.84f, 1.0f};
    ship.pbr.metallic = 0.85f;
    ship.pbr.roughness = 0.34f;
    assets.registerMaterial(ship);

    engine::MaterialDesc rock;
    rock.name = "asteroid";
    rock.baseColor = glm::vec4{0.45f, 0.42f, 0.39f, 1.0f};
    rock.pbr.metallic = 0.05f;
    rock.pbr.roughness = 0.95f;
    assets.registerMaterial(rock);

    // HDR-bright exhaust material: values > 1 push the nozzle pixels
    // into the Phase 22 bloom bright-pass.
    engine::MaterialDesc glow;
    glow.name = "thrusterGlow";
    glow.baseColor = glm::vec4{6.0f, 2.1f, 0.5f, 1.0f};
    glow.pbr.metallic = 0.0f;
    glow.pbr.roughness = 0.4f;
    assets.registerMaterial(glow);
}

entt::entity ensureShip(engine::Scene& scene) {
    if (const entt::entity existing = findByTag(scene, kShipTag); existing != entt::null) {
        return existing;
    }
    engine::Entity ship = scene.createEntity(kShipTag);
    ship.addComponent<engine::TransformComponent>(glm::vec3{0.0f});
    auto& mesh = ship.addComponent<engine::MeshComponent>();
    mesh.assetId = "spacecraft";
    mesh.materialName = "spacecraft";
    mesh.localBounds = engine::AABB{glm::vec3{-0.85f, -0.7f, -2.4f}, glm::vec3{0.85f, 0.7f, 1.6f}};
    auto& body = ship.addComponent<engine::RigidBodyComponent>(kShipMass);
    body.useGravity = false; // Newtonian space: no gravity well
    body.gravityScale = 0.0f;
    body.allowSleep = false; // pilot input must never be gated by sleep
    body.friction = 0.0f;
    body.restitution = 0.0f;
    ship.addComponent<engine::ColliderComponent>(engine::ColliderComponent::box(glm::vec3{0.85f, 0.7f, 2.0f}));
    return ship.handle();
}

void ensureAsteroids(engine::Scene& scene) {
    const size_t existing = countByTag(scene, "Asteroid");
    for (size_t i = existing; i < static_cast<size_t>(kAsteroidCount); ++i) {
        const uint32_t seed = 0x9E3779B9u ^ (static_cast<uint32_t>(i) * 2654435761u);
        const auto rnd = [seed](uint32_t salt) { return hash01(seed + salt * 2246822519u); };
        glm::vec3 direction{rnd(1) * 2.0f - 1.0f, rnd(2) * 2.0f - 1.0f, rnd(3) * 2.0f - 1.0f};
        if (glm::dot(direction, direction) < 1e-4f) {
            direction = glm::vec3{1.0f, 0.3f, 0.2f};
        }
        direction = glm::normalize(direction);
        const glm::vec3 position = direction * (48.0f + 210.0f * rnd(4));
        const float baseScale = 1.5f + 5.5f * rnd(5);
        const glm::vec3 scale{baseScale * (0.7f + 0.6f * rnd(6)), baseScale * (0.7f + 0.6f * rnd(7)),
                              baseScale * (0.7f + 0.6f * rnd(8))};

        engine::Entity asteroid = scene.createEntity("Asteroid");
        auto& transform = asteroid.addComponent<engine::TransformComponent>(position);
        transform.rotationDegrees = glm::vec3{rnd(9) * 360.0f, rnd(10) * 360.0f, rnd(11) * 360.0f};
        transform.scale = scale;
        auto& mesh = asteroid.addComponent<engine::MeshComponent>();
        mesh.assetId = "asteroid";
        mesh.materialName = "asteroid";
        mesh.localBounds = engine::AABB{glm::vec3{-1.4f}, glm::vec3{1.4f}};
        auto& body = asteroid.addComponent<engine::RigidBodyComponent>(0.0f, true);
        body.useGravity = false;
        // PhysicsSystem scales collider dimensions by the transform
        // scale, so the collider carries the unit-mesh radius (the
        // jittered icosahedron reaches ~1.28) and `scale` does the
        // sizing — anything else would double-count the size.
        asteroid.addComponent<engine::ColliderComponent>(engine::ColliderComponent::sphere(1.3f));
        asteroid.addComponent<engine::SpinComponent>(
            glm::vec3{(rnd(12) - 0.5f) * 10.0f, (rnd(13) - 0.5f) * 10.0f, (rnd(14) - 0.5f) * 10.0f});
    }
}

void ensureNozzles(engine::Scene& scene, entt::entity shipHandle) {
    engine::Entity ship(shipHandle, &scene);
    for (const NozzleSpec& spec : kNozzles) {
        if (findByTag(scene, spec.tag) != entt::null) {
            continue;
        }
        engine::Entity nozzle = scene.createEntity(spec.tag);
        nozzle.addComponent<engine::TransformComponent>(spec.offset).scale = glm::vec3{spec.scale};
        auto& mesh = nozzle.addComponent<engine::MeshComponent>();
        mesh.assetId = "cube";
        mesh.materialName = "thrusterGlow";
        auto& light = nozzle.addComponent<engine::PointLightComponent>();
        light.color = spec.color;
        light.intensity = spec.main ? 1.4f : 0.9f;
        light.range = spec.range;
        nozzle.setParent(ship);
    }
}

// --- The flight behaviour, as a native script (Phase 25) ----------------------
//
// One instance per ship, created by ScriptRegistry under the name
// "SpaceFlightScript" and ticked by NativeScriptSystem. Tuning the
// m_params defaults (or exposing them to the reflection-driven
// editor later) + rebuilding this module is the whole workflow.

class SpaceFlightScript final : public engine::INativeScript {
public:
    void onAwake() override {
        if (scene() != nullptr) {
            ensureNozzles(*scene(), entity().handle());
        }
    }

    void onFixedUpdate(float) override {
        if (scene() == nullptr || !hasComponent<engine::RigidBodyComponent>()) {
            return;
        }
        auto& body = getComponent<engine::RigidBodyComponent>();

        FlightInput input;
        if (context().input != nullptr) {
            const engine::Input& keys = *context().input;
            const auto axis = [&keys](engine::Key negative, engine::Key positive) {
                return (keys.isKeyDown(positive) ? 1.0f : 0.0f) - (keys.isKeyDown(negative) ? 1.0f : 0.0f);
            };
            input.main = axis(engine::Key::S, engine::Key::W);
            input.strafe = axis(engine::Key::A, engine::Key::D);
            input.lift = axis(engine::Key::LeftControl, engine::Key::Space);
            input.roll = axis(engine::Key::Q, engine::Key::E);
            // Mouse: right = yaw right (nose right), up = pitch up.
            const glm::vec2 mouse = keys.mouseDelta();
            input.yaw = std::clamp(-mouse.x * m_params.mouseTorqueScale, -1.0f, 1.0f);
            input.pitch = std::clamp(-mouse.y * m_params.mouseTorqueScale, -1.0f, 1.0f);
        }

        const glm::quat orientation = body.orientation;
        // PhysicsSystem tracks angular velocity in degrees/second.
        constexpr float kDegreesToRadians = 0.01745329252f;
        const glm::vec3 angularLocalRad =
            glm::inverse(orientation) * body.angularVelocity * kDegreesToRadians;
        const glm::vec3 inertiaLocal{body.inertiaTensor[0][0], body.inertiaTensor[1][1], body.inertiaTensor[2][2]};

        const FlightForces forces = computeFlightForces(m_params, input, angularLocalRad, inertiaLocal);
        body.addForce(orientation * forces.forceLocal);
        body.addTorque(orientation * forces.torqueLocal);
        m_mainThrottle = forces.mainThrottle;
        m_lateralThrottle = forces.lateralThrottle;
    }

    void onUpdate(float) override {
        if (scene() == nullptr) {
            return;
        }
        // Thruster-nozzle lights pulse with throttle: a hard flicker
        // (~7 Hz) over a throttle-proportional base intensity lights
        // up nearby asteroid rock whenever an engine burns.
        const float flicker = 0.87f + 0.13f * std::sin(static_cast<float>(context().elapsedSeconds) * 43.0f);
        for (const NozzleSpec& spec : kNozzles) {
            const entt::entity nozzle = findByTag(*scene(), spec.tag);
            if (nozzle == entt::null) {
                continue;
            }
            auto& light = scene()->registry().get<engine::PointLightComponent>(nozzle);
            const float throttle = spec.main ? m_mainThrottle : m_lateralThrottle;
            light.intensity = ((spec.main ? 1.4f : 0.9f) + (spec.main ? 30.0f : 15.0f) * throttle) * flicker;
        }
    }

private:
    FlightParams m_params;
    float m_mainThrottle = 0.0f;
    float m_lateralThrottle = 0.0f;
};

engine::INativeScript* createSpaceFlightScript() { return new SpaceFlightScript(); }

} // namespace

REGISTER_SCRIPT(SpaceFlightScript)

namespace {

class SpaceFlightModule final : public engine::IGameModule {
public:
    void onLoad(engine::EngineContext& context) override {
        m_scene = context.scene;
        if (context.scene == nullptr) {
            return;
        }
        if (context.assets != nullptr) {
            registerAssets(*context.assets);
        }
        // Make the flight parameters visible to reflection tooling.
        engine::ReflectionRegistry::instance().registerType(engine::reflect<spaceflight::FlightParams>());
        // Bridge the script class into the host's registry (covers
        // statically linked modules, whose REGISTER_SCRIPT lands in
        // a private registry copy). The named factory's address is
        // this generation's ownership token for unregistration.
        m_scriptRegistry = context.scripts != nullptr ? context.scripts : &engine::ScriptRegistry::instance();
        m_scriptRegistry->registerScript(kFlightScriptName, &createSpaceFlightScript);
        const entt::entity ship = ensureShip(*context.scene);
        ensureAsteroids(*context.scene);
        ensureNozzles(*context.scene, ship);
        // Attach the behaviour; NativeScriptSystem instantiates it
        // by name on the next tick.
        auto& registry = context.scene->registry();
        if (!registry.all_of<engine::NativeScriptComponent>(ship)) {
            registry.emplace<engine::NativeScriptComponent>(ship, kFlightScriptName);
        }
    }

    void onUnload() override {
        // Withdraw this generation's registrations first (pointer-
        // identity checked: a newer generation that already
        // re-registered under the same name is left untouched), so
        // the host never holds factories or reflection tables into
        // a binary that is about to be unmapped.
        if (m_scriptRegistry != nullptr) {
            m_scriptRegistry->unregisterScript(kFlightScriptName, &createSpaceFlightScript);
        }
        engine::ReflectionRegistry::instance().unregisterType(engine::reflect<spaceflight::FlightParams>());
        // The live script instance is owned by this binary and its
        // code is about to be unmapped: destroy it now. The next
        // generation's onLoad re-registers the class and the script
        // system re-creates the instance on the same ship.
        if (m_scene == nullptr) {
            return;
        }
        const entt::entity ship = findByTag(*m_scene, kShipTag);
        if (ship == entt::null) {
            return;
        }
        auto& registry = m_scene->registry();
        if (!registry.all_of<engine::NativeScriptComponent>(ship)) {
            return;
        }
        auto& component = registry.get<engine::NativeScriptComponent>(ship);
        if (component.instance != nullptr) {
            try {
                component.instance->onDestroy();
            } catch (...) {
                // A dying module must never throw across the boundary.
            }
            delete component.instance;
            component.instance = nullptr;
            component.started = false;
        }
    }

    // Per-frame behaviour lives in SpaceFlightScript (driven by
    // NativeScriptSystem); the module only owns load-time setup.
    void onUpdate(engine::EngineContext&, float) override {}
    void onFixedUpdate(engine::EngineContext&, float) override {}

    [[nodiscard]] const char* moduleName() const override { return "SpaceFlightModule"; }

private:
    engine::Scene* m_scene = nullptr;
    engine::ScriptRegistry* m_scriptRegistry = nullptr;
};

} // namespace

ENGINE_IMPLEMENT_GAME_MODULE(SpaceFlightModule)
