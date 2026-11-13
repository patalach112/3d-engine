#pragma once
#include "Core/Export.h"

#include "Core/Camera.h"
#include "Core/HardwareProfile.h"
#include "Geometry/Bounds.h"
#include "Physics/PhysicsComponents.h"

#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <entt/entity/entity.hpp>

#include <string>
#include <vector>

namespace engine {

class INativeScript;

// Human-readable entity name (debugging, future tooling, logs).
struct TagComponent {
    std::string tag;
};

// Position / Euler rotation (degrees, applied Y then X then Z) / scale.
// This is the single source of truth for an entity's placement; systems
// derive world matrices from it, nothing caches transforms elsewhere.
//
// Data-oriented layout: the component is exactly one 64-byte cache line
// (three vec3 fields + tail padding, alignas(64)). EnTT stores components
// in dense paged arrays (1024 per page), and inside those arrays every
// transform starts on a cache-line boundary — aligned loads for
// SIMD-friendly math, and adjacent transforms never share a cache line,
// so parallel workers writing different entities never false-share.
struct alignas(64) TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotationDegrees{0.0f};
    glm::vec3 scale{1.0f};
    // Authoritative orientation for physics-driven bodies. Rigid-body
    // orientation is a quaternion; converting it to Euler angles and back
    // every step (the old path) is lossy near gimbal lock and made the
    // rendered matrix drift/jitter against the physics geometry.
    // PhysicsSystem therefore writes its quaternion straight here and
    // modelMatrix() consumes it 1:1. The quaternion + flag live in the
    // cache line's tail padding, so the 64-byte layout is unchanged.
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    bool useQuaternionOrientation = false;

    TransformComponent() = default;
    explicit TransformComponent(const glm::vec3& position_) : position(position_) {}

    // Stores a physics orientation for rendering: kept normalized, and
    // rotationDegrees is refreshed from it so Euler readers (spawn
    // capture, tooling) still see an equivalent angle triple.
    void setOrientation(const glm::quat& quaternion) {
        orientation = glm::normalize(quaternion);
        useQuaternionOrientation = true;
        rotationDegrees = glm::mod(glm::degrees(glm::eulerAngles(orientation)), 360.0f);
    }

    // Rebuilds the quaternion from rotationDegrees (Euler order Y, X, Z —
    // the same order modelMatrix used historically). For systems that
    // author rotation as Euler angles (RotationSystem spin).
    void syncOrientationFromEuler() {
        const glm::vec3 radians = glm::radians(rotationDegrees);
        orientation = glm::normalize(glm::angleAxis(radians.y, glm::vec3{0.0f, 1.0f, 0.0f}) *
                                     glm::angleAxis(radians.x, glm::vec3{1.0f, 0.0f, 0.0f}) *
                                     glm::angleAxis(radians.z, glm::vec3{0.0f, 0.0f, 1.0f}));
        useQuaternionOrientation = true;
    }

    // Model matrix: T * R * S, where R is the authoritative quaternion
    // when useQuaternionOrientation is set, else R(yaw) * R(pitch) *
    // R(roll) from rotationDegrees.
    [[nodiscard]] ENGINE_SCENE_API glm::mat4 modelMatrix() const;
};

// ---------------------------------------------------------------------------
// Scene-graph hierarchy (Phase 18)
//
// TransformComponent above holds LOCAL placement (relative to the
// parent). These two components encode the tree itself; both sides
// are kept consistent exclusively by Scene::setParent /
// removeParent / destroyEntity — user code must never patch them
// directly. Entities without a ParentComponent are roots.
// ---------------------------------------------------------------------------
struct ParentComponent {
    entt::entity parent = entt::null;
};

struct ChildrenComponent {
    std::vector<entt::entity> children;
};

// Computed world placement, (re)built by TransformSystem from the
// local transform and the ancestor chain:
// worldMatrix = rootLocal * ... * parentLocal * local. Decomposed
// fields serve systems that need position/orientation/scale without
// touching the matrix. Never authored by user code.
struct WorldTransformComponent {
    glm::mat4 worldMatrix{1.0f};
    glm::vec3 worldPosition{0.0f};
    glm::quat worldOrientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 worldScale{1.0f};
};

// Wraps the first-person Camera. The first component with isActive = true
// is the one CameraSystem drives and RenderSystem renders through.
struct CameraComponent {
    Camera camera;
    bool isActive = true;

    CameraComponent() : camera(CameraConfig{}) {}
    explicit CameraComponent(const CameraConfig& config) : camera(config) {}
};

// Dynamic point light (Phase 21): radiates in all directions from
// the entity's world position with smooth distance attenuation
// (windowed inverse-square). Gathered by the LightingSystem into the
// renderer's light arrays; position comes from the entity's
// transform (world-aware through the hierarchy).
struct PointLightComponent {
    glm::vec3 color{1.0f};
    float intensity = 30.0f; // physical-ish scale for the 1/d^2 term
    float range = 12.0f;     // hard cutoff radius in meters
    bool enabled = true;
    // Shadow-casting point light: the renderer budgets ONE cube
    // shadow map (six 90-degree depth faces); the nearest enabled
    // castShadow point packed into the frame's light slots owns it.
    bool castShadow = false;
};

// Dynamic spot light (Phase 21): a cone from the entity's world
// position along its world forward (-Z of the entity orientation,
// the same convention cameras use), fading smoothly between the
// inner and outer cone half-angles.
struct SpotLightComponent {
    glm::vec3 color{1.0f};
    float intensity = 40.0f;
    float range = 18.0f;
    float innerConeRadians = 0.35f; // full-intensity cone half-angle
    float outerConeRadians = 0.6f;  // fade to zero here
    bool enabled = true;
    // Phase 32: this spot casts a real-time shadow (one 2D shadow
    // map is budgeted: the nearest shadow-casting spot packed into
    // the frame's 16 light slots wins).
    bool castShadow = false;
};

// Directional key light (Phase 32): the scene's sun, driving the
// cascaded-shadow directional term in the PBR shader. `direction`
// points TOWARD the light (the convention the shader uses). A
// directional light is infinitely far away, so the entity's
// transform is ignored. When several are enabled, the highest
// intensity wins; with none, the renderer keeps the legacy
// fixed sun (direction 0.35/0.8/0.45, white-warm, intensity 1).
struct DirectionalLightComponent {
    glm::vec3 color{1.0f};
    float intensity = 1.0f; // radiance scale (legacy fixed sun: 1)
    glm::vec3 direction{0.35f, 0.8f, 0.45f}; // toward the light
    bool enabled = true;
    bool castShadow = true;            // sample the cascade shadow maps
    uint32_t cascadeCount = 4;         // CSM slices, clamped to 1..4
};

// Player flashlight (survival-horror staple): a shadow-casting
// spotlight glued to the active camera — positioned at the camera,
// aimed along its view direction every frame by LightingSystem,
// and given priority for the budgeted spot shadow map so the beam
// the player points is always the one that casts real-time shadows
// onto world geometry and dynamic props. Attach to the camera
// entity; hosts toggle `enabled` (the F key in the demo hosts).
struct FlashlightComponent {
    glm::vec3 color{1.0f, 0.97f, 0.90f};
    float intensity = 60.0f;
    float range = 25.0f;            // metres of useful beam
    float innerConeRadians = 0.32f; // full-intensity cone half-angle
    float outerConeRadians = 0.52f; // fade to zero here
    bool enabled = true;
    bool castShadow = true; // owns the spot shadow map while enabled
};

// Placeholder for the future asset system: identifies which mesh the
// entity wants to draw. Until real mesh assets exist, the renderer
// substitutes its built-in cube geometry for any MeshComponent, so the
// assetId is carried but not yet resolved.
struct MeshComponent {
    // Hot fields first (read every frame by the render system); the
    // cold assetId string (with its heap pointer) sits at the back so
    // iteration rarely touches its cache line.
    // Local-space bounds used for frustum culling. Matches the built-in
    // unit cube; real mesh assets will supply their own bounds later.
    AABB localBounds{glm::vec3{-0.5f}, glm::vec3{0.5f}};
    bool visible = true;
    std::string assetId = "cube";    // mesh asset name (AssetManager)
    std::string materialName = "default"; // material name (AssetManager)

    MeshComponent() = default;
    explicit MeshComponent(std::string assetId_) : assetId(std::move(assetId_)) {}
};

// Behaviour attachment (Phase 25): names an INativeScript class
// registered with ScriptRegistry; NativeScriptSystem instantiates
// it on first tick and drives its lifecycle. `scriptName` is the
// serialization identifier — the instance itself is runtime state,
// created and destroyed inside the process (and, for game modules,
// inside the module binary that registered the class). Move-only:
// exactly one component owns the instance.
struct NativeScriptComponent {
    INativeScript* instance = nullptr;
    std::string scriptName;
    bool started = false; // onStart delivered

    NativeScriptComponent() = default;
    explicit NativeScriptComponent(std::string name) : scriptName(std::move(name)) {}
    NativeScriptComponent(const NativeScriptComponent&) = delete;
    NativeScriptComponent& operator=(const NativeScriptComponent&) = delete;
    NativeScriptComponent(NativeScriptComponent&& other) noexcept
        : instance(other.instance), scriptName(std::move(other.scriptName)), started(other.started) {
        other.instance = nullptr;
        other.started = false;
    }
    NativeScriptComponent& operator=(NativeScriptComponent&& other) noexcept {
        if (this != &other) {
            instance = other.instance;
            scriptName = std::move(other.scriptName);
            started = other.started;
            other.instance = nullptr;
            other.started = false;
        }
        return *this;
    }
};


// Continuous rotation, integrated by RotationSystem in fixed updates
// (deterministic spin for props, turrets, pickups, ...).
// Read-mostly during iteration, so it packs densely: exactly 16 bytes,
// four spins per cache line.
struct alignas(16) SpinComponent {
    glm::vec3 degreesPerSecond{0.0f};

    SpinComponent() = default;
    explicit SpinComponent(const glm::vec3& degreesPerSecond_) : degreesPerSecond(degreesPerSecond_) {}
};

// Compile-time layout guarantees for the hot components (see above).
static_assert(sizeof(TransformComponent) == hardware::cacheLineBytes &&
                  alignof(TransformComponent) == hardware::cacheLineBytes,
              "TransformComponent must occupy exactly one cache line");
static_assert(sizeof(SpinComponent) == 16 && alignof(SpinComponent) == 16,
              "SpinComponent must pack four-per-cache-line");

} // namespace engine
