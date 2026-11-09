#pragma once
#include "Core/Export.h"

#include "Physics/PhysicsComponents.h"
#include "Scene/Components.h"

#include <entt/entity/entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace engine {

class JobSystem;
class Scene;

// ---------------------------------------------------------------------------
// PhysicsSystem
//
// Fixed-timestep, data-oriented rigid-body physics for ECS entities with
// TransformComponent + RigidBodyComponent + ColliderComponent.
//
// Pipeline: snapshot bodies/fields/joints/thrusters -> apply thruster and
// spring forces -> parallel semi-implicit integration (linear + quaternion
// rotational dynamics) -> parallel world-geometry construction -> X-axis
// sweep broad phase -> parallel narrow phase/CCD into disjoint slots ->
// deterministic island generation -> warm-started sequential-impulse
// solving per island (islands can solve in parallel; bodies never repeat
// across islands). Scratch storage is retained between steps.
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API PhysicsSystem {
public:
    struct Stats {
        size_t bodies = 0;
        size_t dynamicBodies = 0;
        size_t sleepingBodies = 0;
        size_t candidatePairs = 0;
        size_t contacts = 0;
        size_t islands = 0;
        size_t largestIsland = 0;
        size_t joints = 0;
        uint32_t solverIterations = 0;
        uint32_t subSteps = 1;
    };

    Stats update(Scene& scene, double fixedDeltaSeconds, const PhysicsSettings& settings,
                 JobSystem* jobs = nullptr);
    Stats update(Scene& scene, double fixedDeltaSeconds, const PhysicsSettings& settings, JobSystem& jobs);

    size_t captureSpawnStates(Scene& scene);
    size_t resetToSpawnStates(Scene& scene);

    [[nodiscard]] const Stats& lastStats() const { return m_lastStats; }
    [[nodiscard]] size_t spawnStateCount() const { return m_spawnStates.size(); }

private:
    struct SpawnState {
        entt::entity entity = entt::null;
        glm::vec3 position{0.0f};
        glm::vec3 rotationDegrees{0.0f};
        glm::vec3 scale{1.0f};
        glm::vec3 velocity{0.0f};
        glm::vec3 angularVelocity{0.0f};
        glm::vec3 angularMomentum{0.0f};
        glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 spinDegreesPerSecond{0.0f};
        bool hasSpin = false;
    };

    struct BodyRef {
        entt::entity entity = entt::null;
        TransformComponent* transform = nullptr;
        RigidBodyComponent* body = nullptr;
        ColliderComponent* collider = nullptr;
    };

    struct BodyGeometry {
        ColliderComponent::Type type = ColliderComponent::Type::Box;
        glm::vec3 center{0.0f};
        glm::vec3 previousCenter{0.0f};
        glm::vec3 displacement{0.0f};
        glm::vec3 boundsCenter{0.0f};
        glm::vec3 boundsHalf{0.5f};
        glm::vec3 halfExtents{0.5f};
        glm::mat3 axes{1.0f};
        glm::vec3 planeNormal{0.0f, 1.0f, 0.0f};
        glm::vec3 capsuleAxis{0.0f, 1.0f, 0.0f};
        float radius = 0.5f;
        float capsuleHalfHeight = 0.0f;
        std::array<glm::vec3, 8> points{};
        uint8_t pointCount = 0;
        bool isPlane = false;
    };

    struct BroadEntry {
        float minX = 0.0f;
        float maxX = 0.0f;
        uint32_t bodyIndex = 0;
    };

    struct Contact {
        uint32_t bodyA = 0;
        uint32_t bodyB = 0;
        glm::vec3 normal{1.0f, 0.0f, 0.0f};
        // World-space contact point (solver lever arm). Manifold
        // generation emits up to four of these per pair (box corners,
        // capsule endpoints), so resting stacks are supported across a
        // face instead of balanced on one central point.
        glm::vec3 point{0.0f};
        float penetration = 0.0f;
        float restitution = 0.0f;
        float friction = 0.0f;
        glm::vec2 anisotropicFriction{-1.0f, -1.0f};
        float normalImpulse = 0.0f;
        float restitutionTarget = 0.0f;
        glm::vec3 tangentImpulse{0.0f};
        bool continuous = false;
        float timeOfImpact = 1.0f;
        uint8_t manifoldCount = 1; // contacts sharing this pair (position correction is split across them)
    };

    struct WarmContact {
        uint64_t pairKey = 0;
        glm::vec3 normal{1.0f, 0.0f, 0.0f};
        // Contact point stored in body A's local frame: manifold points
        // are features (box corners, capsule ends), so the local point
        // identifies the feature across steps. Re-applying an impulse at
        // a *different* feature would inject phantom torque, so matching
        // is done on this local point, not the moving world point.
        glm::vec3 localPointA{0.0f};
        float normalImpulse = 0.0f;
        float restitutionTarget = 0.0f;
        glm::vec3 tangentImpulse{0.0f};
    };

    struct GravitySource {
        glm::vec3 position{0.0f};
        float gravitationalParameter = 0.0f;
        float radius = 0.0f;
    };

    struct ThrusterRef {
        uint32_t bodyIndex = 0;
        ThrusterComponent* thruster = nullptr;
    };

    struct JointRef {
        uint32_t bodyA = 0; // entity carrying the JointComponent
        uint32_t bodyB = 0; // JointComponent::other, when present
        JointComponent* joint = nullptr;
        bool hasBodyB = false;
        float restLength = 0.0f; // resolved from a negative component value
    };

    struct SolverIsland {
        std::vector<size_t> contacts;
        std::vector<size_t> joints;
    };

    void snapshotBodies(Scene& scene);
    void snapshotConstraints(Scene& scene);
    void applyExternalForces();
    void integrate(double fixedDeltaSeconds, const PhysicsSettings& settings, JobSystem* jobs);
    void buildGeometry(JobSystem* jobs, uint32_t parallelMinBodies, float dt);
    void broadPhase();
    void narrowPhase(JobSystem* jobs, uint32_t parallelMinBodies);
    void applyContinuousCollisionStops();
    void buildIslands();
    void solveConstraints(const PhysicsSettings& settings, JobSystem* jobs);

    [[nodiscard]] std::optional<Contact> collide(uint32_t bodyA, uint32_t bodyB) const;
    // Multi-point manifold generation: appends up to four contacts for
    // the pair (single-point fallback for shape pairs without a dedicated
    // manifold path).
    void collideManifold(uint32_t bodyA, uint32_t bodyB, std::vector<Contact>& out) const;
    // World-space support point of a body's shape in a direction (used
    // for contact lever arms on shape pairs without a dedicated
    // manifold path).
    [[nodiscard]] glm::vec3 supportPointWorld(uint32_t bodyIndex, const glm::vec3& direction) const;
    // Robust generic contact point: correct depth along the normal
    // (midpoint of the two support faces), tangential position anchored
    // between the body centers — never at a far support corner of a
    // large body, which would fabricate a huge lever arm.
    [[nodiscard]] glm::vec3 contactPointWorld(uint32_t bodyA, uint32_t bodyB, const glm::vec3& normal) const;
    [[nodiscard]] std::optional<Contact> sweptCollide(uint32_t bodyA, uint32_t bodyB, const Contact& base) const;
    void prepareWarmStarting(const PhysicsSettings& settings);
    void storeWarmStarting();
    void applyWarmStart(Contact& contact);
    void resolveVelocity(Contact& contact, const PhysicsSettings& settings);
    void resolvePosition(Contact& contact, const PhysicsSettings& settings);
    // Solver-side mass accessors: sleeping bodies report zero inverse
    // mass (immovable) until woken; point-velocity helpers evaluate
    // v + omega x r at a contact point for torque-coupled impulses.
    [[nodiscard]] float solverInverseMass(uint32_t bodyIndex) const;
    [[nodiscard]] glm::vec3 angularVelocityRad(uint32_t bodyIndex) const;
    [[nodiscard]] glm::vec3 pointVelocity(uint32_t bodyIndex, const glm::vec3& point) const;
    void applyImpulseAtPoint(uint32_t bodyIndex, const glm::vec3& point, const glm::vec3& impulse,
                             float sign);
    void wakeBody(uint32_t bodyIndex);
    void stepOnce(Scene& scene, double fixedDeltaSeconds, const PhysicsSettings& settings, JobSystem* jobs);
    void resolveJointVelocity(const JointRef& joint);
    void resolveJointPosition(const JointRef& joint);
    [[nodiscard]] float inverseMass(uint32_t bodyIndex) const;
    [[nodiscard]] glm::vec3 inverseInertiaWorld(uint32_t bodyIndex, const glm::vec3& vector) const;
    [[nodiscard]] std::optional<uint32_t> bodyIndexFor(entt::entity entity) const;

    std::vector<BodyRef> m_bodies;
    std::vector<BodyGeometry> m_geometry;
    std::vector<BroadEntry> m_broadEntries;
    std::vector<uint32_t> m_planeBodies;
    std::vector<std::pair<uint32_t, uint32_t>> m_candidatePairs;
    std::vector<std::vector<Contact>> m_contactSlots; // one manifold per candidate pair
    std::vector<Contact> m_contacts;
    std::vector<WarmContact> m_warmContacts;
    std::vector<GravitySource> m_gravitySources;
    std::vector<ThrusterRef> m_thrusters;
    std::vector<JointRef> m_joints;
    std::vector<SolverIsland> m_islands;
    std::vector<SpawnState> m_spawnStates;
    // Force/torque accumulated by gameplay code before an update: these
    // act over the whole fixed step, so integrate restores them after
    // each non-final sub-step instead of consuming them in the first.
    std::vector<glm::vec3> m_persistentForces;
    std::vector<glm::vec3> m_persistentTorques;
    uint32_t m_subStepIndex = 0;
    uint32_t m_subStepCount = 1;
    bool m_ccdEnabled = true;
    Stats m_lastStats;
};

} // namespace engine
