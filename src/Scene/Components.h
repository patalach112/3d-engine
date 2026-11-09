#pragma once

#include "Core/Camera.h"
#include "Core/HardwareProfile.h"
#include "Geometry/Bounds.h"
#include "Physics/PhysicsComponents.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <string>

namespace engine {

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

    TransformComponent() = default;
    explicit TransformComponent(const glm::vec3& position_) : position(position_) {}

    // Model matrix: T * R(yaw) * R(pitch) * R(roll) * S.
    [[nodiscard]] glm::mat4 modelMatrix() const;
};

// Wraps the first-person Camera. The first component with isActive = true
// is the one CameraSystem drives and RenderSystem renders through.
struct CameraComponent {
    Camera camera;
    bool isActive = true;

    CameraComponent() : camera(CameraConfig{}) {}
    explicit CameraComponent(const CameraConfig& config) : camera(config) {}
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
