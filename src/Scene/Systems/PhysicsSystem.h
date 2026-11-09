#pragma once

#include "Physics/PhysicsComponents.h"
#include "Scene/Components.h"

#include <entt/entity/entity.hpp>

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
// Fixed-timestep rigid-body physics for ECS entities carrying
// TransformComponent + RigidBodyComponent + ColliderComponent.
//
// Pipeline for one fixed step:
//   1. snapshot the three dense component arrays as a flat body list;
//   2. integrate forces/gravity/velocity/position in parallel;
//   3. build world bounds in parallel and run an X-axis sweep broad-phase;
//   4. evaluate candidate pairs in parallel into disjoint contact slots;
//   5. solve contacts sequentially in deterministic candidate order.
//
// Sequential impulse resolution is deliberate: contacts are coupled, and a
// deterministic solver gives reproducible stacking without atomics or
// contact-graph coloring. All scratch arrays are members and retain their
// capacity between steps.
// ---------------------------------------------------------------------------
class PhysicsSystem {
public:
    struct Stats {
        size_t bodies = 0;
        size_t dynamicBodies = 0;
        size_t candidatePairs = 0;
        size_t contacts = 0;
        uint32_t solverIterations = 0;
    };

    Stats update(Scene& scene, double fixedDeltaSeconds, const PhysicsSettings& settings,
                 JobSystem* jobs = nullptr);
    Stats update(Scene& scene, double fixedDeltaSeconds, const PhysicsSettings& settings, JobSystem& jobs);

    // Captures the current transform/velocity state of every dynamic
    // physics body as its respawn state. Call after building a scene.
    // Existing snapshots are replaced; the storage capacity is retained.
    size_t captureSpawnStates(Scene& scene);

    // Restores every captured dynamic body to its spawn transform and
    // initial linear/angular velocity, clearing acceleration and forces.
    // Entities are neither created nor destroyed, and stale/destroyed
    // handles are skipped. Returns the number of bodies reset.
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
        glm::vec3 spinDegreesPerSecond{0.0f};
        bool hasSpin = false;
    };

    struct BodyRef {
        TransformComponent* transform = nullptr;
        RigidBodyComponent* body = nullptr;
        ColliderComponent* collider = nullptr;
    };

    struct BodyGeometry {
        glm::vec3 center{0.0f};
        glm::vec3 halfExtents{0.5f};
        glm::vec3 planeNormal{0.0f, 1.0f, 0.0f};
        float radius = 0.5f;
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
        glm::vec3 normal{1.0f, 0.0f, 0.0f}; // from body A to body B
        float penetration = 0.0f;
        float restitution = 0.0f;
        float friction = 0.0f;
    };

    void snapshotBodies(Scene& scene);
    void integrate(double fixedDeltaSeconds, const PhysicsSettings& settings, JobSystem* jobs);
    void buildGeometry(JobSystem* jobs, uint32_t parallelMinBodies);
    void broadPhase();
    void narrowPhase(JobSystem* jobs, uint32_t parallelMinBodies);
    void solveContacts(const PhysicsSettings& settings);

    [[nodiscard]] std::optional<Contact> collide(uint32_t bodyA, uint32_t bodyB) const;
    void resolveVelocity(const Contact& contact, const PhysicsSettings& settings);
    void resolvePosition(const Contact& contact, const PhysicsSettings& settings);
    [[nodiscard]] float inverseMass(uint32_t bodyIndex) const;

    std::vector<BodyRef> m_bodies;
    std::vector<BodyGeometry> m_geometry;
    std::vector<BroadEntry> m_broadEntries;
    std::vector<uint32_t> m_planeBodies;
    std::vector<std::pair<uint32_t, uint32_t>> m_candidatePairs;
    std::vector<std::optional<Contact>> m_contactSlots;
    std::vector<Contact> m_contacts;
    std::vector<SpawnState> m_spawnStates;
    Stats m_lastStats;
};

} // namespace engine
