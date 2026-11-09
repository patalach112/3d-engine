#pragma once
#include "Core/Export.h"

#include "Core/EventBus.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsEvents.h"
#include "Scene/Components.h"
#include "Scene/DynamicAABBTree.h"

#include <entt/entity/entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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
// rotational dynamics) -> parallel world-geometry construction -> dynamic
// AABB tree broad phase -> parallel narrow phase/CCD into disjoint slots ->
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

    // Event output (Phase 17): when a bus is attached, the end of
    // each update() publishes CollisionEvent for contact pairs that
    // appeared since the previous update and TriggerEvent for
    // trigger-volume crossings. Null (the default) disables
    // publication; the pair bookkeeping runs either way, so
    // attaching a bus mid-run does not replay history.
    void setEventBus(EventBus* bus) { m_eventBus = bus; }

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

    struct Contact {
        uint32_t bodyA = 0;
        uint32_t bodyB = 0;
        glm::vec3 normal{1.0f, 0.0f, 0.0f};
        // World-space contact point (solver lever arm). Manifold
        // generation emits up to four of these per pair (box corners,
        // capsule endpoints), so resting stacks are supported across a
        // face instead of balanced on one central point.
        glm::vec3 point{0.0f};
        // Pre-centroid manifold feature point (a real corner/endpoint).
        // The velocity solve acts at the manifold centroid (contact.point),
        // but warm-start identity must follow the feature: after centroid
        // reduction every point of a pair shares one point, so matching
        // warm impulses by contact.point would hand every contact the
        // first contact's share and apply it manifoldCount times.
        glm::vec3 featurePoint{0.0f};
        float penetration = 0.0f;
        float restitution = 0.0f;
        float friction = 0.0f;
        glm::vec2 anisotropicFriction{-1.0f, -1.0f};
        float normalImpulse = 0.0f;
        float restitutionTarget = 0.0f;
        glm::vec3 tangentImpulse{0.0f};
        // True when warm starting matched this contact to the previous
        // sub-step's manifold: a persistent contact. Restitution is
        // suppressed for persistent contacts (only new impacts bounce).
        bool persistent = false;
        // Solver inverse masses for this sub-step (shock
        // propagation, assigned in solveConstraints): identical to
        // the bodies' solver inverse masses, except that the
        // shallower body of a support chain (smaller support depth)
        // is treated as immovable within this contact. Gauss-Seidel
        // transfers only ~invM_heavy/invM_light of a heavy body's
        // approach per sweep through a light pinned body, so without
        // this a heavy body tunnels through a light supported one
        // before the iterations converge; with it, support reactions
        // propagate through the chain in one sweep per level.
        // solveInvMass* drive the positional pass (all ratios: the
        // correction share must follow the support, or a crushed
        // chain deadlocks with both corrections saturated against
        // the sandwiched body); velInvMass* drive the velocity pass
        // and are pinned only at extreme mass ratios (>= 16:1), where
        // iteration convergence is hopeless — comparable-mass
        // contacts keep symmetric two-body impulses, which resting
        // stacks rely on for torque balance.
        float solveInvMassA = 0.0f;
        float solveInvMassB = 0.0f;
        float velInvMassA = 0.0f;
        float velInvMassB = 0.0f;
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
        entt::entity jointEntity = entt::null; // key into m_jointStates
        bool hasBodyB = false;
        float restLength = 0.0f; // resolved (explicit, or captured once)
    };

    // Persistent per-joint solver state, keyed by the joint-owning
    // entity: warm-start impulses (accumulated, clamped during the
    // velocity solve) and one-time captures (rest length and the
    // reference relative orientation that hinge limits and fixed
    // joints measure their error against). Lives across sub-steps
    // and steps; pruned when a joint disappears.
    struct JointSolverState {
        float distanceImpulse = 0.0f;      // Distance row accumulator
        glm::vec3 pointImpulse{0.0f};      // Hinge/Fixed anchor rows (world axes)
        glm::vec2 axisImpulse{0.0f};       // Hinge axis-alignment rows
        glm::vec3 angularImpulse{0.0f};    // Fixed orientation rows (world axes)
        float limitImpulse = 0.0f;         // Hinge twist-limit row
        bool restCaptured = false;
        float capturedRestLength = 0.0f;
        bool orientationCaptured = false;
        glm::quat capturedRelativeOrientation{1.0f, 0.0f, 0.0f, 0.0f}; // inverse(qA) * qB
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
    void solveConstraints(const PhysicsSettings& settings, JobSystem* jobs, float dt);
    // Bleeds residual linear/angular energy out of low-speed bodies in
    // contact (see PhysicsSettings::contactLinearDamping) and snaps
    // sub-threshold remainders to exact rest.
    void applyRestingContactDamping(const PhysicsSettings& settings, float dt);

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
    void prepareJointWarmStarting();
    void resolveJointVelocity(const JointRef& joint, float dt);
    void resolveJointPosition(const JointRef& joint);
    // One scalar sequential-impulse row along `direction` acting at
    // the two anchor points, with the accumulated impulse clamped to
    // [minAccumulated, maxAccumulated] (bilateral rows pass infinite
    // bounds; unilateral bound/limit rows clamp one side to zero).
    // biasVelocity is the Baumgarte error feedback (beta * C / dt).
    void solveJointLinearRow(const JointRef& ref, const glm::vec3& pointA, const glm::vec3& pointB,
                             const glm::vec3& direction, float biasVelocity, float& accumulated,
                             float minAccumulated, float maxAccumulated);
    // The angular twin: a row on the relative angular velocity along
    // a world axis, for hinge axis alignment, hinge limits and fixed
    // orientation locking.
    void solveJointAngularRow(const JointRef& ref, const glm::vec3& axis, float biasVelocity,
                              float& accumulated, float minAccumulated, float maxAccumulated);
    void rotateJointBody(uint32_t bodyIndex, const glm::quat& worldDelta);
    [[nodiscard]] float inverseMass(uint32_t bodyIndex) const;
    [[nodiscard]] glm::vec3 inverseInertiaWorld(uint32_t bodyIndex, const glm::vec3& vector) const;
    [[nodiscard]] std::optional<uint32_t> bodyIndexFor(entt::entity entity) const;

    std::vector<BodyRef> m_bodies;
    std::vector<BodyGeometry> m_geometry;
    // Broadphase acceleration: a persistent dynamic AABB tree over the
    // non-plane bodies' world bounds, keyed by entity (entt integral).
    // Fat boxes absorb small motion, so steady scenes only refit on
    // escape; planes (infinite extent) bypass the tree and, as before,
    // are paired against every body. m_broadBodyIndex maps the entity
    // to this step's body index for the query callbacks.
    DynamicAABBTree m_broadTree;
    std::unordered_map<uint32_t, DynamicAABBTree::ProxyId> m_broadProxies;
    std::unordered_map<uint32_t, uint32_t> m_broadBodyIndex;
    std::vector<uint32_t> m_planeBodies;
    std::vector<std::pair<uint32_t, uint32_t>> m_candidatePairs;
    std::vector<std::vector<Contact>> m_contactSlots; // one manifold per candidate pair
    std::vector<Contact> m_contacts;
    std::vector<WarmContact> m_warmContacts;
    std::vector<GravitySource> m_gravitySources;
    std::vector<ThrusterRef> m_thrusters;
    std::vector<JointRef> m_joints;
    std::unordered_map<uint32_t, JointSolverState> m_jointStates; // key: joint entity integral
    // Event publication state: entity-pair sets (packed entity
    // integrals) as of the previous update, diffed against the
    // current sets at the end of update() to derive began/entered
    // and ended/exited transitions.
    EventBus* m_eventBus = nullptr;
    std::unordered_set<uint64_t> m_activeContactPairs;
    std::unordered_set<uint64_t> m_activeTriggerPairs;
    std::vector<uint64_t> m_stepTriggerPairs; // trigger pairs of the latest sub-step
    std::vector<char> m_triggerPairFlags;     // per-candidate-pair trigger marks (narrow phase)
    void publishPhysicsEvents(Scene& scene);
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
