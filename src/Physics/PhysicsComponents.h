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
// Collision layers & masks (Phase 15)
//
// Every collider lives on exactly one layer (a single bit) and carries
// a mask of the layers it is willing to collide with. Two colliders
// interact only when each one's layer appears in the other's mask —
// the check is mutual, so either side can opt out of a pairing (a
// one-way "I see you" is never enough). Filtering happens in the
// broadphase, before any contact manifold or solver work, and the
// same layer values filter Scene raycast queries.
//
// The named categories below cover the common cases; games may use
// any of the 32 bits. Defaults preserve the pre-layer behaviour
// exactly: layer Default, mask All — everything collides with
// everything.
// ---------------------------------------------------------------------------
namespace CollisionLayer {
inline constexpr uint32_t None = 0u;
inline constexpr uint32_t Default = 1u << 0;
inline constexpr uint32_t Player = 1u << 1;
inline constexpr uint32_t Enemy = 1u << 2;
inline constexpr uint32_t Environment = 1u << 3;
inline constexpr uint32_t Trigger = 1u << 4;
inline constexpr uint32_t All = 0xFFFFFFFFu;
} // namespace CollisionLayer

// The mutual layer/mask test: A collides with B iff A's layer is in
// B's mask AND B's layer is in A's mask.
[[nodiscard]] constexpr bool shouldCollide(uint32_t layerA, uint32_t maskA, uint32_t layerB, uint32_t maskB) {
    return (layerA & maskB) != 0u && (layerB & maskA) != 0u;
}

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
    // Collision filtering: the single layer this collider belongs to
    // and the set of layers it collides with (see CollisionLayer).
    uint32_t layer = CollisionLayer::Default;
    uint32_t mask = CollisionLayer::All;

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

// Component-level form of the mutual layer/mask test above.
[[nodiscard]] constexpr bool shouldCollide(const ColliderComponent& a, const ColliderComponent& b) {
    return shouldCollide(a.layer, a.mask, b.layer, b.mask);
}

// A localized radial gravity source. Attach with TransformComponent to a
// planet/field entity; no RigidBodyComponent is required.
struct GravityFieldComponent {
    float gravitationalParameter = 0.0f; // mu = G*M, in m^3/s^2
    float radius = 0.0f;                 // 0 means unbounded
    bool enabled = true;
};

enum class JointType : uint8_t { Distance, Hinge, Fixed, SpringDamper };

// ---------------------------------------------------------------------------
// JointComponent (Phase 16: constraints & joints)
//
// A constraint between the entity carrying this component (body A) and
// `other` (body B), or the world when `other` is null (anchorB and
// hingeAxis are then world-space). Anchors are points in each body's
// local frame; hingeAxis is the rotation axis in both bodies' local
// frames. Joint types:
//
//   Distance    — holds the anchor separation at restLength (a rigid
//                 rod), or inside [minDistance, maxDistance] when
//                 bounds are set (a rope/cable when only maxDistance
//                 is given). Negative restLength captures the
//                 separation when the solver first sees the joint.
//   Hinge       — a revolute joint: the anchor points coincide and
//                 the bodies can only rotate relative to each other
//                 about hingeAxis (doors, pendulums). enableLimits
//                 clamps the twist angle about the axis, measured in
//                 degrees from the relative orientation captured when
//                 the solver first sees the joint.
//   Fixed       — a weld: relative position and orientation are
//                 locked to the configuration captured when the
//                 solver first sees the joint.
//   SpringDamper— a force spring between the anchors (stiffness in
//                 N/m, damping in N*s/m); the only non-rigid type,
//                 applied as an external force before integration.
//
// `compliance` (0..1) softens the rigid types: 0 is a hard constraint,
// higher values scale down the solver's error feedback so the joint
// yields elastically. All rigid types are solved with sequential
// impulses (full point Jacobians with angular terms, effective-mass
// denominators, accumulated-impulse clamping) and are warm-started
// across sub-steps from persistent per-joint impulse state.
// ---------------------------------------------------------------------------
struct JointComponent {
    entt::entity other = entt::null;
    JointType type = JointType::Distance;
    glm::vec3 anchorA{0.0f};
    glm::vec3 anchorB{0.0f};
    glm::vec3 hingeAxis{0.0f, 1.0f, 0.0f};
    float restLength = -1.0f; // Distance: negative captures the current separation
    float minDistance = -1.0f; // Distance: lower bound (negative: unbounded below)
    float maxDistance = -1.0f; // Distance: upper bound (negative: unbounded above)
    float minAngleDegrees = -180.0f; // Hinge twist limits (from the captured zero)
    float maxAngleDegrees = 180.0f;
    bool enableLimits = false; // Hinge: enforce the twist limits
    float compliance = 0.0f;   // rigid types: 0 hard .. 1 very soft
    float stiffness = 100.0f;  // SpringDamper: N/m
    float damping = 5.0f;      // SpringDamper: N*s/m
    bool enabled = true;

    [[nodiscard]] static JointComponent distance(entt::entity other_, const glm::vec3& anchorA_,
                                                 const glm::vec3& anchorB_, float restLength_) {
        JointComponent joint;
        joint.other = other_;
        joint.type = JointType::Distance;
        joint.anchorA = anchorA_;
        joint.anchorB = anchorB_;
        joint.restLength = restLength_;
        return joint;
    }

    [[nodiscard]] static JointComponent hinge(entt::entity other_, const glm::vec3& anchorA_,
                                              const glm::vec3& anchorB_, const glm::vec3& axis_) {
        JointComponent joint;
        joint.other = other_;
        joint.type = JointType::Hinge;
        joint.anchorA = anchorA_;
        joint.anchorB = anchorB_;
        joint.hingeAxis = axis_;
        return joint;
    }

    [[nodiscard]] static JointComponent fixed(entt::entity other_, const glm::vec3& anchorA_,
                                              const glm::vec3& anchorB_) {
        JointComponent joint;
        joint.other = other_;
        joint.type = JointType::Fixed;
        joint.anchorA = anchorA_;
        joint.anchorB = anchorB_;
        return joint;
    }

    [[nodiscard]] static JointComponent springDamper(entt::entity other_, const glm::vec3& anchorA_,
                                                     const glm::vec3& anchorB_, float restLength_,
                                                     float stiffness_, float damping_) {
        JointComponent joint;
        joint.other = other_;
        joint.type = JointType::SpringDamper;
        joint.anchorA = anchorA_;
        joint.anchorB = anchorB_;
        joint.restLength = restLength_;
        joint.stiffness = stiffness_;
        joint.damping = damping_;
        return joint;
    }
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
    float angularDamping = 0.0f;
    // Approach speeds at or below this (m/s) never produce restitution:
    // the contact rests instead of bouncing. Restitution additionally
    // only fires on a *new* impact — a persistent (warm-started)
    // contact never re-bounces, which is what stops stacked bodies
    // from launching each other in a restitution chain reaction.
    float restitutionVelocityThreshold = 1.0f;
    float contactSlop = 0.003f;
    // Relaxed positional gain: each pass removes only this fraction of
    // the remaining penetration (clamped per pass below), so resting
    // manifolds converge instead of popping apart and re-impacting.
    float positionCorrection = 0.35f;
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
    float maxPositionCorrection = 0.02f;
    // Resting-contact damping (exponential rates, 1/s): a body resting
    // on a persistent, weight-bearing contact whose speeds are already
    // residual (below 0.3 m/s and 90 deg/s) bleeds linear/angular
    // energy every sub-step, and sub-0.01 m/s / sub-1 deg/s remainders
    // snap to exact zero. This is what drains micro-jitter out of
    // stacks instead of letting solver noise circulate forever.
    // Bodies that opt out of sleep (the player) are never damped.
    float contactLinearDamping = 20.0f;
    float contactAngularDamping = 40.0f;
    // Sleeping: bodies slower than both thresholds for sleepDelaySeconds
    // fall asleep (skipped by integration, immovable in the solver).
    // The thresholds are deliberately tight — a body moving faster
    // than this is visibly moving, not jittering.
    bool sleepEnabled = true;
    float sleepSpeedThreshold = 0.05f;       // m/s
    float sleepAngularThreshold = 3.0f;       // degrees/s
    float sleepDelaySeconds = 0.5f;
    float wakeSpeedThreshold = 0.2f;          // m/s approach speed that wakes a sleeping body
};

static_assert(sizeof(RigidBodyComponent) == 208 && alignof(RigidBodyComponent) == 16,
              "RigidBodyComponent must remain a dense 208-byte value");
static_assert(sizeof(ColliderComponent) == 160 && alignof(ColliderComponent) == 16,
              "ColliderComponent must remain a dense 160-byte value");

} // namespace engine
