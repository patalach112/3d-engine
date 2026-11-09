#pragma once

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <entt/entity/entity.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace engine {

// ---------------------------------------------------------------------------
// RigidBodyComponent
//
// Dynamic state for one rigid body. TransformComponent remains the source
// of truth for rendered position/scale; PhysicsSystem publishes this
// component's authoritative quaternion into the transform each step, so
// the rendered model matrix is built from the exact simulated rotation.
//
// Rotational state is physical rather than a simple Euler spin:
// angularMomentum is the conserved world-space quantity, the full local
// inertia tensor (and inverse) describes mass distribution, and
// angularVelocity is derived as omega = I_world^-1 * L. In the absence of
// torque this produces torque-free precession/gyroscopic motion while L
// itself remains constant.
// ---------------------------------------------------------------------------
struct alignas(16) RigidBodyComponent {
    glm::vec3 velocity{0.0f};
    glm::vec3 angularVelocity{0.0f};  // degrees/second, derived from momentum
    glm::vec3 angularMomentum{0.0f};  // world-space conserved quantity
    glm::vec3 acceleration{0.0f};     // acceleration used by the most recent step
    glm::vec3 force{0.0f};            // accumulated force, cleared after integration
    glm::vec3 torque{0.0f};           // accumulated torque, cleared after integration
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::mat3 inertiaTensor{1.0f};
    glm::mat3 inverseInertiaTensor{1.0f};

    float mass = 1.0f;
    float inverseMass = 1.0f;
    float restitution = 0.35f;
    float friction = 0.45f;
    // Optional anisotropic friction in the two deterministic contact
    // tangent directions. A negative component means "use friction".
    glm::vec2 anisotropicFriction{-1.0f, -1.0f};
    float gravityScale = 1.0f;
    bool isStatic = false;
    bool useGravity = true;
    bool autoInertia = true;
    bool orientationInitialized = false;
    // Sleep state (PhysicsSystem): a body whose linear/angular speeds
    // stay below the settings thresholds for sleepDelaySeconds falls
    // asleep and is skipped by integration and treated as immovable by
    // the solver until a force, impulse-worthy impact, or joint partner
    // wakes it. The player body opts out (allowSleep = false) so input
    // response is never gated by sleep.
    float sleepTimer = 0.0f;
    bool sleeping = false;
    bool allowSleep = true;

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
            angularMomentum = glm::vec3{0.0f};
        }
    }

    void setInertiaTensor(const glm::mat3& inertia) {
        inertiaTensor = inertia;
        const float determinant = glm::determinant(inertia);
        inverseInertiaTensor = std::abs(determinant) > 1e-12f ? glm::inverse(inertia) : glm::mat3{0.0f};
        autoInertia = false;
    }

    void setPrincipalInertia(const glm::vec3& moments) {
        setInertiaTensor(glm::mat3{moments.x, 0.0f, 0.0f, 0.0f, moments.y, 0.0f, 0.0f, 0.0f, moments.z});
    }

    void addForce(const glm::vec3& addedForce) {
        if (!isStatic) {
            sleeping = false; // external forces wake the body
            sleepTimer = 0.0f;
            force += addedForce;
        }
    }

    void addTorque(const glm::vec3& addedTorque) {
        if (!isStatic) {
            sleeping = false;
            sleepTimer = 0.0f;
            torque += addedTorque;
        }
    }

    void applyForceAtPoint(const glm::vec3& appliedForce, const glm::vec3& worldPoint,
                           const glm::vec3& worldCenterOfMass) {
        addForce(appliedForce);
        addTorque(glm::cross(worldPoint - worldCenterOfMass, appliedForce));
    }

    void clearForces() { force = glm::vec3{0.0f}; }
    void clearTorques() { torque = glm::vec3{0.0f}; }
    void clearForcesAndTorques() {
        clearForces();
        clearTorques();
    }

    [[nodiscard]] bool dynamic() const { return !isStatic && mass > 0.0f; }
};

// ---------------------------------------------------------------------------
// ColliderComponent
//
// Primitive/convex collider description. Dimensions are local and are
// scaled by TransformComponent::scale. Box is treated as an OBB once the
// body rotates. Capsules use the local Y axis. Convex hulls accept up to
// eight points and collide through world-space support/SAT projection.
//
// centerOffset shifts the collider's center in the body's local frame
// (also scaled by TransformComponent::scale). It exists so a collider
// can sit exactly on an imported mesh's AABB center when the model's
// origin is not its geometric center — the rendered mesh and the
// hitbox then occupy the same world volume with no manual tuning.
// ---------------------------------------------------------------------------
struct alignas(16) ColliderComponent {
    enum class Type : uint8_t { Box, Sphere, Plane, Obb, Capsule, ConvexHull };

    Type type = Type::Box;
    glm::vec3 halfExtents{0.5f, 0.5f, 0.5f};
    glm::vec3 centerOffset{0.0f};
    float radius = 0.5f;
    float capsuleHalfHeight = 0.25f; // half length of the cylinder segment
    glm::vec3 planeNormal{0.0f, 1.0f, 0.0f};
    float planeOffset = 0.0f;
    std::array<glm::vec3, 8> hullPoints{};
    uint8_t hullPointCount = 0;

    ColliderComponent() = default;
    explicit ColliderComponent(const glm::vec3& halfExtents_) : halfExtents(halfExtents_) {}

    [[nodiscard]] static ColliderComponent box(const glm::vec3& halfExtents_,
                                               const glm::vec3& centerOffset_ = glm::vec3{0.0f}) {
        ColliderComponent collider;
        collider.type = Type::Box;
        collider.halfExtents = halfExtents_;
        collider.centerOffset = centerOffset_;
        return collider;
    }

    [[nodiscard]] static ColliderComponent obb(const glm::vec3& halfExtents_,
                                               const glm::vec3& centerOffset_ = glm::vec3{0.0f}) {
        ColliderComponent collider = box(halfExtents_, centerOffset_);
        collider.type = Type::Obb;
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

    [[nodiscard]] static ColliderComponent capsule(float radius_, float halfHeight) {
        ColliderComponent collider;
        collider.type = Type::Capsule;
        collider.radius = radius_;
        collider.capsuleHalfHeight = halfHeight;
        return collider;
    }

    [[nodiscard]] static ColliderComponent convexHull(std::span<const glm::vec3> points) {
        ColliderComponent collider;
        collider.type = Type::ConvexHull;
        const size_t count = std::min(points.size(), collider.hullPoints.size());
        for (size_t i = 0; i < count; ++i) {
            collider.hullPoints[i] = points[i];
        }
        collider.hullPointCount = static_cast<uint8_t>(count);
        return collider;
    }
};

// Import/spawning code refers to the box configuration of this unified
// collider component by this conventional name (ColliderComponent::box).
using BoxColliderComponent = ColliderComponent;

// A localized radial gravity source. Attach with TransformComponent to a
// planet/field entity; no RigidBodyComponent is required.
struct GravityFieldComponent {
    float gravitationalParameter = 0.0f; // mu = G*M, in m^3/s^2
    float radius = 0.0f;                 // 0 means unbounded
    bool enabled = true;
};

enum class JointType : uint8_t { Distance, Hinge, SpringDamper };

// Constraint between the entity carrying this component and `other`.
struct JointComponent {
    entt::entity other = entt::null;
    JointType type = JointType::Distance;
    glm::vec3 anchorA{0.0f};
    glm::vec3 anchorB{0.0f};
    glm::vec3 hingeAxis{0.0f, 1.0f, 0.0f};
    float restLength = -1.0f; // negative: capture the current distance
    float stiffness = 100.0f; // SpringDamper: N/m
    float damping = 5.0f;     // SpringDamper: N*s/m
    bool enabled = true;
};

// Local-space propulsion. Force is applied at localPosition, so offset
// thrusters naturally generate torque as well as linear acceleration.
struct ThrusterComponent {
    glm::vec3 force{0.0f};
    glm::vec3 torque{0.0f};
    glm::vec3 localPosition{0.0f};
    float throttle = 1.0f;
    bool enabled = true;
};

enum class PhysicsDemoMode : uint8_t { Terrestrial, Space };

// Physics tuning (see [Physics] in config/engine.ini).
struct PhysicsSettings {
    bool enabled = true;
    PhysicsDemoMode demoMode = PhysicsDemoMode::Terrestrial;
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    glm::vec3 gravityCenter{0.0f};       // used when gravitationalParameter > 0
    float gravitationalParameter = 0.0f; // optional global radial field (mu)
    float gravitySoftening = 0.01f;      // avoids a singularity at a point mass
    float linearDamping = 0.0f;
    float restitutionVelocityThreshold = 1.0f;
    float contactSlop = 0.001f;
    float positionCorrection = 0.8f;
    uint32_t solverIterations = 4;
    uint32_t parallelMinBodies = 128;
    bool ccdEnabled = true;
    // Internal sub-stepping: each update() call is split into this many
    // full simulation sub-steps, so fast bodies (player capsule, CCD
    // projectiles) are sampled several times per fixed step and cannot
    // tunnel through thin geometry between samples.
    uint32_t subSteps = 4;
    // Non-linear position correction: the positional pass iterates,
    // re-measuring the remaining penetration after every correction, and
    // each iteration's separation gain is clamped to
    // maxPositionCorrection meters so deep landings resolve firmly over
    // a few passes instead of exploding in one.
    uint32_t positionIterations = 2;
    float maxPositionCorrection = 0.05f;
    // Sleeping: bodies slower than both thresholds for sleepDelaySeconds
    // fall asleep (skipped by integration, immovable in the solver).
    bool sleepEnabled = true;
    float sleepSpeedThreshold = 0.12f;       // m/s
    float sleepAngularThreshold = 8.0f;       // degrees/s
    float sleepDelaySeconds = 0.5f;
    float wakeSpeedThreshold = 0.2f;          // m/s approach speed that wakes a sleeping body
};

static_assert(sizeof(RigidBodyComponent) == 208 && alignof(RigidBodyComponent) == 16,
              "RigidBodyComponent must remain a dense 208-byte value");
static_assert(sizeof(ColliderComponent) == 160 && alignof(ColliderComponent) == 16,
              "ColliderComponent must remain a dense 160-byte value");

} // namespace engine
