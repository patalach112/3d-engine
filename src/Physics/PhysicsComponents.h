#pragma once

#include "Core/HardwareProfile.h"

#include <glm/vec3.hpp>

#include <algorithm>
#include <cstdint>

namespace engine {

// ---------------------------------------------------------------------------
// RigidBodyComponent
//
// Dynamic state for one rigid body. TransformComponent remains the single
// source of truth for position; this component owns velocity and transient
// force/acceleration state. Forces are accumulated between fixed steps,
// consumed by PhysicsSystem, and cleared by that step.
//
// EnTT stores components in dense paged arrays, so bodies are iterated as
// packed data rather than through gameplay-object pointer graphs. The hot
// value packs all linear/angular state into five cache lines' worth of
// four bodies (80 bytes each, 16-byte aligned).
// ---------------------------------------------------------------------------
struct alignas(16) RigidBodyComponent {
    glm::vec3 velocity{0.0f};
    glm::vec3 angularVelocity{0.0f}; // degrees per second, by Euler axis
    glm::vec3 acceleration{0.0f};    // acceleration used by the most recent step
    glm::vec3 force{0.0f};           // accumulated force, cleared after integration

    float mass = 1.0f;
    float inverseMass = 1.0f;
    float restitution = 0.35f; // 0 = inelastic, 1 = perfectly elastic
    float friction = 0.45f;    // Coulomb friction coefficient
    bool isStatic = false;
    bool useGravity = true;

    RigidBodyComponent() = default;
    explicit RigidBodyComponent(float mass_, bool isStatic_ = false) : isStatic(isStatic_) { setMass(mass_); }

    void setMass(float mass_) {
        mass = std::max(mass_, 0.0f);
        inverseMass = (isStatic || mass <= 0.0f) ? 0.0f : 1.0f / mass;
    }

    void setStatic(bool static_) {
        isStatic = static_;
        inverseMass = static_ ? 0.0f : (mass > 0.0f ? 1.0f / mass : 0.0f);
        if (isStatic) {
            velocity = glm::vec3{0.0f};
            angularVelocity = glm::vec3{0.0f};
        }
    }

    void addForce(const glm::vec3& addedForce) {
        if (!isStatic) {
            force += addedForce;
        }
    }

    void clearForces() { force = glm::vec3{0.0f}; }

    [[nodiscard]] bool dynamic() const { return !isStatic && mass > 0.0f; }
};

// ---------------------------------------------------------------------------
// ColliderComponent
//
// Primitive collider description. Box half-extents and sphere radius are
// in the entity's local space and are scaled by TransformComponent::scale
// by PhysicsSystem. A plane is n*x = planeOffset, with planeNormal facing
// the free half-space; planes are normally attached to static bodies.
// ---------------------------------------------------------------------------
struct alignas(16) ColliderComponent {
    enum class Type : uint8_t { Box, Sphere, Plane };

    Type type = Type::Box;
    glm::vec3 halfExtents{0.5f, 0.5f, 0.5f};
    float radius = 0.5f;
    glm::vec3 planeNormal{0.0f, 1.0f, 0.0f};
    float planeOffset = 0.0f;

    ColliderComponent() = default;
    explicit ColliderComponent(const glm::vec3& halfExtents_) : halfExtents(halfExtents_) {}

    [[nodiscard]] static ColliderComponent box(const glm::vec3& halfExtents_) {
        ColliderComponent collider;
        collider.type = Type::Box;
        collider.halfExtents = halfExtents_;
        return collider;
    }

    [[nodiscard]] static ColliderComponent sphere(float radius_) {
        ColliderComponent collider;
        collider.type = Type::Sphere;
        collider.radius = radius_;
        return collider;
    }

    [[nodiscard]] static ColliderComponent plane(const glm::vec3& normal, float offset) {
        ColliderComponent collider;
        collider.type = Type::Plane;
        collider.planeNormal = normal;
        collider.planeOffset = offset;
        return collider;
    }
};

// Physics tuning (see [Physics] in config/engine.ini).
struct PhysicsSettings {
    bool enabled = true;
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float linearDamping = 0.0f; // velocity fraction removed per second
    // Impacts slower than this are treated as resting contacts (no bounce),
    // which is essential for stable stacks without visible jitter.
    float restitutionVelocityThreshold = 1.0f;
    float contactSlop = 0.001f;          // allowed penetration, in world units
    float positionCorrection = 0.8f;     // fraction of excess penetration corrected per step
    uint32_t solverIterations = 4;
    uint32_t parallelMinBodies = 128;    // below this, sequential batches are cheaper
};

static_assert(sizeof(RigidBodyComponent) == 80 && alignof(RigidBodyComponent) == 16,
              "RigidBodyComponent must remain a dense 80-byte value");
static_assert(sizeof(ColliderComponent) == 48 && alignof(ColliderComponent) == 16,
              "ColliderComponent must remain a dense 48-byte value");

} // namespace engine
