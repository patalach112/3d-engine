#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include "Scene/Systems/PhysicsSystem.h"

#include "Physics/MovementSystem.h"
#include "Scene/Systems/TransformSystem.h"

#include "Core/Jobs/JobSystem.h"
#include "Scene/Scene.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace engine {
namespace {

constexpr float kEpsilon = 1e-6f;

// Shortest-arc rotation vector of a quaternion (2*sin(theta/2) * axis,
// sign-normalized): the orientation error measure the fixed/hinge
// angular rows feed back on.
[[nodiscard]] glm::vec3 quatErrorVector(const glm::quat& quaternion) {
    glm::quat normalized = glm::normalize(quaternion);
    if (normalized.w < 0.0f) {
        normalized = glm::quat{-normalized.w, -normalized.x, -normalized.y, -normalized.z};
    }
    return 2.0f * glm::vec3{normalized.x, normalized.y, normalized.z};
}

void quatToAxisAngle(const glm::quat& quaternion, glm::vec3& axis, float& angle) {
    glm::quat normalized = glm::normalize(quaternion);
    if (normalized.w < 0.0f) {
        normalized = glm::quat{-normalized.w, -normalized.x, -normalized.y, -normalized.z};
    }
    const float halfSin = std::sqrt(std::max(0.0f, 1.0f - normalized.w * normalized.w));
    angle = 2.0f * std::atan2(halfSin, normalized.w);
    axis = halfSin > 1e-8f ? glm::vec3{normalized.x, normalized.y, normalized.z} / halfSin
                           : glm::vec3{1.0f, 0.0f, 0.0f};
}

// Twist of the relative orientation (current vs captured reference)
// about the hinge axis, in degrees, in [-180, 180].
[[nodiscard]] float twistAngleDegrees(const glm::quat& relative, const glm::quat& captured,
                                      const glm::vec3& axisLocal) {
    glm::quat delta = glm::normalize(relative * glm::inverse(captured));
    if (delta.w < 0.0f) {
        delta = glm::quat{-delta.w, -delta.x, -delta.y, -delta.z};
    }
    const float twist = glm::dot(glm::vec3{delta.x, delta.y, delta.z}, axisLocal);
    return glm::degrees(2.0f * std::atan2(twist, delta.w));
}

// Orthonormal basis perpendicular to a unit axis.
void perpendicularBasis(const glm::vec3& axis, glm::vec3& tangent1, glm::vec3& tangent2) {
    const glm::vec3 reference = std::abs(axis.y) < 0.9f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
    tangent1 = glm::normalize(glm::cross(axis, reference));
    tangent2 = glm::cross(axis, tangent1);
}

[[nodiscard]] float combinedRestitution(const RigidBodyComponent& a, const RigidBodyComponent& b) {
    return std::sqrt(std::clamp(a.restitution, 0.0f, 1.0f) * std::clamp(b.restitution, 0.0f, 1.0f));
}

[[nodiscard]] float combinedFriction(const RigidBodyComponent& a, const RigidBodyComponent& b) {
    return std::sqrt(std::max(a.friction, 0.0f) * std::max(b.friction, 0.0f));
}

[[nodiscard]] glm::vec2 combinedAnisotropicFriction(const RigidBodyComponent& a, const RigidBodyComponent& b) {
    const auto component = [](float value, float fallback) { return value >= 0.0f ? value : fallback; };
    return {std::sqrt(component(a.anisotropicFriction.x, a.friction) * component(b.anisotropicFriction.x, b.friction)),
            std::sqrt(component(a.anisotropicFriction.y, a.friction) * component(b.anisotropicFriction.y, b.friction))};
}

[[nodiscard]] glm::quat quaternionFromEulerDegrees(const glm::vec3& degrees) {
    const glm::vec3 radians = glm::radians(degrees);
    return glm::quat_cast(glm::eulerAngleYXZ(radians.y, radians.x, radians.z));
}

[[nodiscard]] glm::mat3 rotationMatrix(const glm::quat& orientation) {
    return glm::mat3_cast(glm::normalize(orientation));
}

[[nodiscard]] glm::mat3 inertiaForCollider(float mass, const ColliderComponent& collider, const glm::vec3& scale) {
    if (mass <= 0.0f || collider.type == ColliderComponent::Type::Plane) {
        return glm::mat3{0.0f};
    }
    switch (collider.type) {
    case ColliderComponent::Type::Sphere: {
        const float radius = collider.radius * std::max({scale.x, scale.y, scale.z});
        const float moment = 0.4f * mass * radius * radius;
        return glm::mat3{moment, 0.0f, 0.0f, 0.0f, moment, 0.0f, 0.0f, 0.0f, moment};
    }
    case ColliderComponent::Type::Capsule: {
        const float radius = collider.radius * std::max({scale.x, scale.z});
        const float halfHeight = collider.capsuleHalfHeight * scale.y;
        const float axial = 0.5f * mass * radius * radius;
        const float transverse = mass * (0.375f * radius * radius + halfHeight * halfHeight / 3.0f);
        return glm::mat3{transverse, 0.0f, 0.0f, 0.0f, axial, 0.0f, 0.0f, 0.0f, transverse};
    }
    case ColliderComponent::Type::ConvexHull: {
        if (collider.hullPointCount == 0) {
            break;
        }
        glm::vec3 minPoint = collider.hullPoints[0];
        glm::vec3 maxPoint = collider.hullPoints[0];
        for (uint8_t i = 1; i < collider.hullPointCount; ++i) {
            minPoint = glm::min(minPoint, collider.hullPoints[i]);
            maxPoint = glm::max(maxPoint, collider.hullPoints[i]);
        }
        const glm::vec3 dimensions = (maxPoint - minPoint) * scale;
        const float factor = mass / 12.0f;
        return glm::mat3{factor * (dimensions.y * dimensions.y + dimensions.z * dimensions.z), 0.0f, 0.0f,
                         0.0f, factor * (dimensions.x * dimensions.x + dimensions.z * dimensions.z), 0.0f,
                         0.0f, 0.0f, factor * (dimensions.x * dimensions.x + dimensions.y * dimensions.y)};
    }
    case ColliderComponent::Type::Box:
    case ColliderComponent::Type::Obb:
    case ColliderComponent::Type::Plane:
        break;
    }
    const glm::vec3 dimensions = collider.halfExtents * 2.0f * scale;
    const float factor = mass / 12.0f;
    glm::mat3 inertia{factor * (dimensions.y * dimensions.y + dimensions.z * dimensions.z), 0.0f, 0.0f,
                      0.0f, factor * (dimensions.x * dimensions.x + dimensions.z * dimensions.z), 0.0f,
                      0.0f, 0.0f, factor * (dimensions.x * dimensions.x + dimensions.y * dimensions.y)};
    // Parallel-axis term: a box whose center is offset from the body
    // origin (imported-mesh hitboxes) resists rotation about that
    // origin with the extra m * (|d|^2 I - d d^T) inertia.
    const glm::vec3 offset = collider.centerOffset * scale;
    const float offsetLengthSq = glm::dot(offset, offset);
    if (offsetLengthSq > 1e-12f) {
        inertia += mass * glm::mat3{offsetLengthSq - offset.x * offset.x, -offset.y * offset.x,
                                    -offset.z * offset.x, -offset.x * offset.y,
                                    offsetLengthSq - offset.y * offset.y, -offset.z * offset.y,
                                    -offset.x * offset.z, -offset.y * offset.z,
                                    offsetLengthSq - offset.z * offset.z};
    }
    return inertia;
}


// Closest points between two segments (standard segment/segment distance;
// used for exact capsule/capsule narrow phase).
void closestSegmentSegment(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2,
                           glm::vec3& closest1, glm::vec3& closest2) {
    const glm::vec3 d1 = q1 - p1;
    const glm::vec3 d2 = q2 - p2;
    const glm::vec3 r = p1 - p2;
    const float a = glm::dot(d1, d1);
    const float e = glm::dot(d2, d2);
    const float f = glm::dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= kEpsilon && e <= kEpsilon) {
        closest1 = p1;
        closest2 = p2;
        return;
    }
    if (a <= kEpsilon) {
        t = glm::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = glm::dot(d1, r);
        if (e <= kEpsilon) {
            s = glm::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2);
            const float denominator = a * e - b * b;
            s = denominator > kEpsilon ? glm::clamp((b * f - c * e) / denominator, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = glm::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = glm::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    closest1 = p1 + d1 * s;
    closest2 = p2 + d2 * t;
}

// Distance from a segment to an axis-aligned box in box-local space. The
// squared distance along a segment to a convex box is convex in the segment
// parameter, so a deterministic ternary search finds the exact closest
// parameter (48 iterations shrink the interval far below float precision).
float segmentBoxDistance(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& halfExtents, float& outT,
                         glm::vec3& outSegmentPoint, glm::vec3& outBoxPoint) {
    const auto distanceAt = [&p0, &p1, &halfExtents](float t, glm::vec3& segmentPoint, glm::vec3& boxPoint) {
        segmentPoint = p0 + (p1 - p0) * t;
        boxPoint = glm::clamp(segmentPoint, -halfExtents, halfExtents);
        const glm::vec3 delta = segmentPoint - boxPoint;
        return glm::dot(delta, delta);
    };
    float low = 0.0f;
    float high = 1.0f;
    glm::vec3 unusedA;
    glm::vec3 unusedB;
    for (int iteration = 0; iteration < 48; ++iteration) {
        const float third = (high - low) / 3.0f;
        const float midLow = low + third;
        const float midHigh = high - third;
        if (distanceAt(midLow, unusedA, unusedB) < distanceAt(midHigh, unusedA, unusedB)) {
            high = midHigh;
        } else {
            low = midLow;
        }
    }
    outT = (low + high) * 0.5f;
    return std::sqrt(distanceAt(outT, outSegmentPoint, outBoxPoint));
}

void initializeOrientationAndInertia(TransformComponent& transform, RigidBodyComponent& body,
                                     ColliderComponent& collider) {
    if (!body.orientationInitialized) {
        body.orientation = quaternionFromEulerDegrees(transform.rotationDegrees);
        body.orientationInitialized = true;
    }
    if (glm::dot(body.orientation, body.orientation) > kEpsilon) {
        body.orientation = glm::normalize(body.orientation);
    } else {
        body.orientation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    if (body.autoInertia && body.dynamic()) {
        body.inertiaTensor = inertiaForCollider(body.mass, collider, glm::abs(transform.scale));
        const float determinant = glm::determinant(body.inertiaTensor);
        body.inverseInertiaTensor = std::abs(determinant) > 1e-12f ? glm::inverse(body.inertiaTensor) : glm::mat3{0.0f};
    }
}

} // namespace

PhysicsSystem::Stats PhysicsSystem::update(Scene& scene, double fixedDeltaSeconds,
                                           const PhysicsSettings& settings, JobSystem& jobs) {
    return update(scene, fixedDeltaSeconds, settings, &jobs);
}

PhysicsSystem::Stats PhysicsSystem::update(Scene& scene, double fixedDeltaSeconds,
                                           const PhysicsSettings& settings, JobSystem* jobs) {
    m_lastStats = {};
    if (!settings.enabled || fixedDeltaSeconds <= 0.0) {
        return m_lastStats;
    }

    // Scene-graph hierarchy (Phase 18): the solver below works
    // purely in world space, so bodies parented under other
    // entities are converted local -> world here and restored
    // world -> local before this function returns. Outside
    // update(), a TransformComponent always holds authored LOCAL
    // placement (spawn capture/reset keep their local semantics).
    convertParentedBodiesToWorld(scene);

    // Internal sub-stepping: the caller's fixed step is split into
    // settings.subSteps full simulation passes. Every sub-step
    // re-integrates, re-derives world geometry (including the swept CCD
    // displacement), and re-solves, so a fast capsule or projectile is
    // collision-sampled subSteps times per fixed step and cannot tunnel
    // through thin floors/walls between samples. Warm-start impulses and
    // sleep timers carry across sub-steps, so stability compounds
    // instead of resetting.
    const uint32_t subSteps = std::max(1u, settings.subSteps);
    const double subDelta = fixedDeltaSeconds / static_cast<double>(subSteps);
    m_lastStats.subSteps = subSteps;
    m_subStepCount = subSteps;
    for (uint32_t subStep = 0; subStep < subSteps; ++subStep) {
        m_subStepIndex = subStep;
        stepOnce(scene, subDelta, settings, jobs);
    }
    m_subStepIndex = 0;
    m_subStepCount = 1;
    restoreParentedBodiesToLocal(scene);
    publishPhysicsEvents(scene);
    return m_lastStats;
}

void PhysicsSystem::stepOnce(Scene& scene, double fixedDeltaSeconds, const PhysicsSettings& settings,
                             JobSystem* jobs) {
    m_ccdEnabled = settings.ccdEnabled;
    snapshotBodies(scene);
    if (m_subStepIndex == 0) {
        // Capture gameplay-accumulated forces before constraint forces
        // (thrusters/springs, re-applied every sub-step) are added.
        m_persistentForces.assign(m_bodies.size(), glm::vec3{0.0f});
        m_persistentTorques.assign(m_bodies.size(), glm::vec3{0.0f});
        for (size_t i = 0; i < m_bodies.size(); ++i) {
            m_persistentForces[i] = m_bodies[i].body->force;
            m_persistentTorques[i] = m_bodies[i].body->torque;
        }
    }
    snapshotConstraints(scene);
    if (settings.gravitationalParameter > 0.0f) {
        m_gravitySources.push_back(GravitySource{settings.gravityCenter, settings.gravitationalParameter, 0.0f});
    }
    applyExternalForces();
    m_lastStats.bodies = m_bodies.size();
    m_lastStats.dynamicBodies = 0;
    m_lastStats.sleepingBodies = 0;
    for (const BodyRef& ref : m_bodies) {
        if (ref.body->dynamic()) {
            ++m_lastStats.dynamicBodies;
            if (ref.body->sleeping) {
                ++m_lastStats.sleepingBodies;
            }
        }
    }

    integrate(fixedDeltaSeconds, settings, jobs);
    buildGeometry(jobs, settings.parallelMinBodies, static_cast<float>(fixedDeltaSeconds));
    broadPhase();
    narrowPhase(jobs, settings.parallelMinBodies);
    applyContinuousCollisionStops();
    buildIslands();
    solveConstraints(settings, jobs, static_cast<float>(fixedDeltaSeconds));

    // Peak across sub-steps: a contact that exists only in the first
    // sub-step (e.g. a bounce) is still this update's contact report.
    m_lastStats.candidatePairs = std::max(m_lastStats.candidatePairs, m_candidatePairs.size());
    m_lastStats.contacts = std::max(m_lastStats.contacts, m_contacts.size());
    m_lastStats.joints = m_joints.size();
    m_lastStats.solverIterations = settings.solverIterations;
}

size_t PhysicsSystem::captureSpawnStates(Scene& scene) {
    m_spawnStates.clear();
    auto& registry = scene.registry();
    auto view = registry.view<TransformComponent, RigidBodyComponent, ColliderComponent>();
    m_spawnStates.reserve(view.size_hint());

    size_t dynamicCount = 0;
    for (const entt::entity entity : view) {
        const TransformComponent& transform = view.get<TransformComponent>(entity);
        RigidBodyComponent& body = view.get<RigidBodyComponent>(entity);
        TransformComponent& mutableTransform = view.get<TransformComponent>(entity);
        ColliderComponent& mutableCollider = view.get<ColliderComponent>(entity);
        initializeOrientationAndInertia(mutableTransform, body, mutableCollider);
        SpawnState state;
        state.entity = entity;
        state.position = transform.position;
        state.rotationDegrees = transform.rotationDegrees;
        state.scale = transform.scale;
        state.velocity = body.velocity;
        state.angularVelocity = body.angularVelocity;
        state.angularMomentum = body.angularMomentum;
        state.orientation = body.orientation;
        if (registry.all_of<SpinComponent>(entity)) {
            state.hasSpin = true;
            state.spinDegreesPerSecond = registry.get<SpinComponent>(entity).degreesPerSecond;
        }
        m_spawnStates.push_back(state);
        if (body.dynamic()) {
            ++dynamicCount;
        }
    }
    return dynamicCount;
}

size_t PhysicsSystem::resetToSpawnStates(Scene& scene) {
    auto& registry = scene.registry();
    size_t resetCount = 0;

    for (const SpawnState& state : m_spawnStates) {
        if (!registry.valid(state.entity) ||
            !registry.all_of<TransformComponent, RigidBodyComponent, ColliderComponent>(state.entity)) {
            continue; // destroyed or structurally changed since capture: skip safely
        }

        RigidBodyComponent& body = registry.get<RigidBodyComponent>(state.entity);
        if (!body.dynamic()) {
            continue; // static floor/planes are never respawned
        }

        TransformComponent& transform = registry.get<TransformComponent>(state.entity);
        transform.position = state.position;
        transform.rotationDegrees = state.rotationDegrees;
        transform.setOrientation(state.orientation); // rendered matrix matches the restored body
        transform.scale = state.scale;
        body.velocity = state.velocity;
        body.angularVelocity = state.angularVelocity;
        body.angularMomentum = state.angularMomentum;
        body.orientation = state.orientation;
        body.orientationInitialized = true;
        body.sleeping = false;
        body.sleepTimer = 0.0f;
        body.acceleration = glm::vec3{0.0f};
        body.clearForcesAndTorques();

        if (registry.all_of<SpinComponent>(state.entity)) {
            registry.get<SpinComponent>(state.entity).degreesPerSecond =
                state.hasSpin ? state.spinDegreesPerSecond : glm::vec3{0.0f};
        }
        ++resetCount;
    }

    // Contact state from the pre-reset world is meaningless now. Clearing
    // (but not reallocating) the scratch arrays lets the next fixed step
    // rebuild it from the respawned transforms.
    m_candidatePairs.clear();
    m_jointStates.clear();
    m_activeContactPairs.clear();
    m_activeTriggerPairs.clear();
    m_stepTriggerPairs.clear();
    m_broadTree.clear();
    m_broadProxies.clear();
    m_broadBodyIndex.clear();
    m_contactSlots.clear();
    m_contacts.clear();
    m_warmContacts.clear();
    m_lastStats = {};
    return resetCount;
}

void PhysicsSystem::snapshotBodies(Scene& scene) {
    m_bodies.clear();
    auto view = scene.registry().view<TransformComponent, RigidBodyComponent, ColliderComponent>();
    m_bodies.reserve(view.size_hint());
    for (const entt::entity entity : view) {
        BodyRef ref;
        ref.entity = entity;
        ref.transform = &view.get<TransformComponent>(entity);
        ref.body = &view.get<RigidBodyComponent>(entity);
        ref.collider = &view.get<ColliderComponent>(entity);
        // Character-driven bodies (CharacterControllerComponent):
        // PlayerSystem's capsule sweep authors their transform and
        // velocity; the solver treats them as immovable (never
        // integrates, never corrects) so no solver feedback loop
        // can ever launch the character. Contacts still generate
        // and still push OTHER bodies (cube pushback).
        ref.characterDriven = scene.registry().all_of<CharacterControllerComponent>(entity);
        initializeOrientationAndInertia(*ref.transform, *ref.body, *ref.collider);
        m_bodies.push_back(ref);
    }
}

void PhysicsSystem::snapshotConstraints(Scene& scene) {
    auto& registry = scene.registry();
    m_gravitySources.clear();
    m_thrusters.clear();
    m_joints.clear();

    auto gravityView = registry.view<TransformComponent, GravityFieldComponent>();
    for (const entt::entity entity : gravityView) {
        const GravityFieldComponent& field = gravityView.get<GravityFieldComponent>(entity);
        if (field.enabled && field.gravitationalParameter > 0.0f) {
            m_gravitySources.push_back(GravitySource{gravityView.get<TransformComponent>(entity).position,
                                                     field.gravitationalParameter, field.radius});
        }
    }

    auto thrusterView = registry.view<ThrusterComponent, RigidBodyComponent>();
    for (const entt::entity entity : thrusterView) {
        if (const std::optional<uint32_t> index = bodyIndexFor(entity); index.has_value()) {
            m_thrusters.push_back(ThrusterRef{*index, &thrusterView.get<ThrusterComponent>(entity)});
        }
    }
    std::sort(m_thrusters.begin(), m_thrusters.end(),
              [](const ThrusterRef& a, const ThrusterRef& b) { return a.bodyIndex < b.bodyIndex; });

    auto jointView = registry.view<JointComponent>();
    for (const entt::entity entity : jointView) {
        JointComponent& joint = jointView.get<JointComponent>(entity);
        if (!joint.enabled) {
            continue;
        }
        const std::optional<uint32_t> bodyA = bodyIndexFor(entity);
        if (!bodyA.has_value()) {
            continue;
        }
        JointRef ref;
        ref.bodyA = *bodyA;
        ref.joint = &joint;
        ref.jointEntity = entity;
        const glm::vec3 pointA = m_bodies[ref.bodyA].transform->position +
                                 m_bodies[ref.bodyA].body->orientation * joint.anchorA;
        glm::vec3 pointB = joint.anchorB;
        if (joint.other != entt::null) {
            const std::optional<uint32_t> bodyB = bodyIndexFor(joint.other);
            if (!bodyB.has_value() || *bodyB == *bodyA) {
                continue;
            }
            ref.bodyB = *bodyB;
            ref.hasBodyB = true;
            pointB = m_bodies[ref.bodyB].transform->position +
                     m_bodies[ref.bodyB].body->orientation * joint.anchorB;
        }
        // One-time captures live in the persistent solver state: a
        // negative rest length measures the separation ONCE (not
        // every step, which would make the error identically zero),
        // and fixed joints / hinge limits remember the relative
        // orientation they were created in.
        JointSolverState& state = m_jointStates[entt::to_integral(entity)];
        if (joint.restLength >= 0.0f) {
            ref.restLength = joint.restLength;
        } else {
            if (!state.restCaptured) {
                state.capturedRestLength = glm::length(pointB - pointA);
                state.restCaptured = true;
            }
            ref.restLength = state.capturedRestLength;
        }
        if ((joint.type == JointType::Fixed || (joint.type == JointType::Hinge && joint.enableLimits)) &&
            !state.orientationCaptured) {
            const glm::quat orientationA = m_bodies[ref.bodyA].body->orientation;
            const glm::quat orientationB =
                ref.hasBodyB ? m_bodies[ref.bodyB].body->orientation : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            state.capturedRelativeOrientation = glm::normalize(glm::inverse(orientationA) * orientationB);
            state.orientationCaptured = true;
        }
        m_joints.push_back(ref);
    }
    // Prune solver state for joints that vanished (destroyed,
    // disabled, or no longer attached to a body).
    {
        std::vector<uint32_t> liveKeys;
        liveKeys.reserve(m_joints.size());
        for (const JointRef& ref : m_joints) {
            liveKeys.push_back(entt::to_integral(ref.jointEntity));
        }
        std::sort(liveKeys.begin(), liveKeys.end());
        for (auto it = m_jointStates.begin(); it != m_jointStates.end();) {
            if (!std::binary_search(liveKeys.begin(), liveKeys.end(), it->first)) {
                it = m_jointStates.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::sort(m_joints.begin(), m_joints.end(), [](const JointRef& a, const JointRef& b) {
        const uint32_t aB = a.hasBodyB ? a.bodyB : 0u;
        const uint32_t bB = b.hasBodyB ? b.bodyB : 0u;
        if (a.bodyA != b.bodyA) return a.bodyA < b.bodyA;
        if (aB != bB) return aB < bB;
        if (a.joint->type != b.joint->type) return a.joint->type < b.joint->type;
        return a.restLength < b.restLength;
    });
}

void PhysicsSystem::applyExternalForces() {
    for (const ThrusterRef& ref : m_thrusters) {
        ThrusterComponent& thruster = *ref.thruster;
        RigidBodyComponent& body = *m_bodies[ref.bodyIndex].body;
        if (!thruster.enabled || !body.dynamic()) {
            continue;
        }
        const glm::vec3 worldForce = body.orientation * (thruster.force * thruster.throttle);
        const glm::vec3 worldPoint = m_bodies[ref.bodyIndex].transform->position +
                                     body.orientation * thruster.localPosition;
        body.applyForceAtPoint(worldForce, worldPoint, m_bodies[ref.bodyIndex].transform->position);
        body.addTorque(body.orientation * (thruster.torque * thruster.throttle));
    }

    for (const JointRef& ref : m_joints) {
        JointComponent& joint = *ref.joint;
        if (joint.type != JointType::SpringDamper) {
            continue;
        }
        RigidBodyComponent& bodyA = *m_bodies[ref.bodyA].body;
        if (!bodyA.dynamic()) {
            continue;
        }
        const glm::vec3 pointA = m_bodies[ref.bodyA].transform->position + bodyA.orientation * joint.anchorA;
        glm::vec3 pointB = joint.anchorB;
        glm::vec3 velocityB{0.0f};
        if (ref.hasBodyB) {
            RigidBodyComponent& bodyB = *m_bodies[ref.bodyB].body;
            pointB = m_bodies[ref.bodyB].transform->position + bodyB.orientation * joint.anchorB;
            velocityB = bodyB.velocity;
        }
        const glm::vec3 axis = pointB - pointA;
        const float distance = glm::length(axis);
        if (distance <= kEpsilon) {
            continue;
        }
        const glm::vec3 direction = axis / distance;
        const float stretch = distance - ref.restLength;
        const float stretchRate = glm::dot(velocityB - bodyA.velocity, direction);
        const glm::vec3 force = direction * (joint.stiffness * stretch + joint.damping * stretchRate);
        bodyA.addForce(force);
        if (ref.hasBodyB) {
            RigidBodyComponent& bodyB = *m_bodies[ref.bodyB].body;
            if (bodyB.dynamic()) {
                bodyB.addForce(-force);
            }
        }
    }
}

void PhysicsSystem::integrate(double fixedDeltaSeconds, const PhysicsSettings& settings, JobSystem* jobs) {
    const float dt = static_cast<float>(fixedDeltaSeconds);
    const auto integrateRange = [this, dt, &settings](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            BodyRef& ref = m_bodies[i];
            RigidBodyComponent& body = *ref.body;
            const auto consumeForces = [this, i](RigidBodyComponent& target) {
                if (m_subStepIndex + 1 >= m_subStepCount) {
                    target.clearForcesAndTorques();
                } else if (i < m_persistentForces.size()) {
                    target.force = m_persistentForces[i];
                    target.torque = m_persistentTorques[i];
                }
            };
            if (!body.dynamic()) {
                body.inverseMass = 0.0f;
                body.acceleration = glm::vec3{0.0f};
                consumeForces(body);
                continue;
            }
            if (ref.characterDriven) {
                // Controller-authored: the capsule sweep in
                // PlayerSystem already produced this step's
                // position and velocity. Never integrate it, never
                // apply gravity/torque, never sleep it — and via
                // solverInverseMass the solver cannot correct it
                // either. Penetration feedback (the "launched out
                // of the map" bug class) has no path to this body.
                consumeForces(body);
                continue;
            }
            initializeOrientationAndInertia(*ref.transform, body, *ref.collider);

            if (body.sleeping) {
                // Sleeping bodies skip integration entirely (the CPU
                // saving). They wake on accumulated force/torque, or
                // when gravity applies but last step's contacts no
                // longer support them (support removed underneath).
                const bool hasForce = glm::dot(body.force, body.force) > kEpsilon ||
                                      glm::dot(body.torque, body.torque) > kEpsilon;
                bool supported = !body.useGravity;
                if (!supported) {
                    for (const Contact& contact : m_contacts) {
                        if (contact.bodyA == static_cast<uint32_t>(i) ||
                            contact.bodyB == static_cast<uint32_t>(i)) {
                            supported = true;
                            break;
                        }
                    }
                }
                if (hasForce || !supported) {
                    body.sleeping = false;
                    body.sleepTimer = 0.0f;
                } else {
                    body.velocity = glm::vec3{0.0f};
                    body.angularVelocity = glm::vec3{0.0f};
                    body.angularMomentum = glm::vec3{0.0f};
                    body.acceleration = glm::vec3{0.0f};
                    consumeForces(body);
                    continue;
                }
            }

            glm::vec3 gravitationalAcceleration = settings.gravity;
            for (const GravitySource& source : m_gravitySources) {
                const glm::vec3 offset = source.position - ref.transform->position;
                const float distanceSq = glm::dot(offset, offset);
                if (distanceSq <= kEpsilon) {
                    continue;
                }
                const float distance = std::sqrt(distanceSq);
                const float softenedDistanceSq = distanceSq + settings.gravitySoftening * settings.gravitySoftening;
                glm::vec3 attraction = offset / distance * (source.gravitationalParameter / softenedDistanceSq);
                if (source.radius > 0.0f) {
                    const float falloff = glm::clamp(1.0f - distance / source.radius, 0.0f, 1.0f);
                    attraction *= falloff * falloff * (3.0f - 2.0f * falloff);
                }
                gravitationalAcceleration += attraction;
            }
            if (!body.useGravity) {
                gravitationalAcceleration = glm::vec3{0.0f};
            } else {
                gravitationalAcceleration *= body.gravityScale;
            }

            body.inverseMass = 1.0f / body.mass;
            body.acceleration = gravitationalAcceleration + body.force * body.inverseMass;
            body.velocity += body.acceleration * dt;

            const float linearDamping = glm::clamp(settings.linearDamping, 0.0f, 1.0f);
            if (linearDamping > 0.0f) {
                body.velocity *= std::max(0.0f, 1.0f - linearDamping * dt);
            }

            // Integrate in angular radians internally, expose degrees on the
            // component. Angular momentum is the conserved world quantity;
            // velocity is derived back from the current world inertia.
            glm::vec3 angularVelocity = glm::radians(body.angularVelocity);
            if (glm::dot(body.angularMomentum, body.angularMomentum) <= kEpsilon && glm::length(angularVelocity) > kEpsilon) {
                const glm::mat3 worldInertia = rotationMatrix(body.orientation) * body.inertiaTensor * glm::transpose(rotationMatrix(body.orientation));
                body.angularMomentum = worldInertia * angularVelocity;
            }
            body.angularMomentum += body.torque * dt;
            const float angularDamping = glm::clamp(settings.angularDamping, 0.0f, 1.0f);
            if (angularDamping > 0.0f) {
                body.angularMomentum *= std::max(0.0f, 1.0f - angularDamping * dt);
            }
            angularVelocity = inverseInertiaWorld(static_cast<uint32_t>(i), body.angularMomentum);

            const float angularSpeed = glm::length(angularVelocity);
            if (angularSpeed > kEpsilon) {
                const glm::vec3 axis = angularVelocity / angularSpeed;
                const glm::quat delta = glm::angleAxis(angularSpeed * dt, axis);
                body.orientation = glm::normalize(delta * body.orientation);
            }
            body.angularVelocity = glm::degrees(angularVelocity);

            // Sleep bookkeeping: sustained slowness (both linear and
            // angular) accumulates toward sleep; any real motion resets
            // the timer. Falling asleep zeroes the velocities so the
            // body is perfectly at rest instead of micro-jittering.
            if (settings.sleepEnabled && body.allowSleep) {
                const float linearSpeed = glm::length(body.velocity);
                const float angularSpeed = glm::length(body.angularVelocity);
                // Sleep requires a resting contact (last sub-step's
                // contact list) or a gravity-free body: a slow but
                // unsupported body (falling apex, orbiting craft) must
                // never freeze mid-flight.
                bool restingContact = !body.useGravity;
                if (!restingContact) {
                    for (const Contact& contact : m_contacts) {
                        if (contact.bodyA == static_cast<uint32_t>(i) ||
                            contact.bodyB == static_cast<uint32_t>(i)) {
                            restingContact = true;
                            break;
                        }
                    }
                }
                if (restingContact && linearSpeed < settings.sleepSpeedThreshold &&
                    angularSpeed < settings.sleepAngularThreshold) {
                    body.sleepTimer += dt;
                    if (body.sleepTimer >= settings.sleepDelaySeconds) {
                        body.sleeping = true;
                        body.velocity = glm::vec3{0.0f};
                        body.angularVelocity = glm::vec3{0.0f};
                        body.angularMomentum = glm::vec3{0.0f};
                    }
                } else {
                    // Decay instead of hard reset: resting stacks jitter
                    // in small bursts (contact flicker, warm-start
                    // transients); a burst should drain the timer, not
                    // restart the whole sleep delay.
                    body.sleepTimer = std::max(0.0f, body.sleepTimer - 2.0f * dt);
                }
            } else {
                body.sleepTimer = 0.0f;
            }

            ref.transform->position += body.velocity * dt;
            // 1:1 visual sync: push the integrated quaternion straight
            // into the transform. The render matrix is built from this
            // exact quaternion (no Euler round-trip), so what is drawn
            // is exactly what the collider solver simulated.
            ref.transform->setOrientation(body.orientation);
            consumeForces(body);
        }
    };

    if (jobs != nullptr && m_bodies.size() >= settings.parallelMinBodies) {
        jobs->parallelFor(m_bodies.size(), 0, integrateRange);
    } else {
        integrateRange(0, m_bodies.size());
    }
}

void PhysicsSystem::buildGeometry(JobSystem* jobs, uint32_t parallelMinBodies, float dt) {
    m_geometry.resize(m_bodies.size());
    const auto buildRange = [this, dt](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            BodyRef& ref = m_bodies[i];
            BodyGeometry& geometry = m_geometry[i];
            const ColliderComponent& collider = *ref.collider;
            RigidBodyComponent& body = *ref.body;
            geometry.type = collider.type;
            geometry.axes = rotationMatrix(body.orientation);
            const glm::vec3 scale = glm::abs(ref.transform->scale);
            // The collider's world center rides its local centerOffset
            // (scaled, then rotated into world space): an imported
            // mesh's hitbox sits on its AABB center even when the model
            // origin — the transform/body position, which stays the
            // center of mass for impulse lever arms — is elsewhere.
            geometry.center = ref.transform->position + geometry.axes * (collider.centerOffset * scale);
            geometry.previousCenter = geometry.center - body.velocity * dt;
            geometry.displacement = body.velocity * dt;
            geometry.isPlane = collider.type == ColliderComponent::Type::Plane;
            geometry.boundsCenter = geometry.center;

            geometry.halfExtents = collider.halfExtents * scale;
            geometry.radius = collider.radius * std::max({scale.x, scale.y, scale.z});
            geometry.planeNormal = glm::dot(collider.planeNormal, collider.planeNormal) > kEpsilon
                                      ? glm::normalize(collider.planeNormal)
                                      : glm::vec3{0.0f, 1.0f, 0.0f};
            geometry.capsuleAxis = geometry.axes * glm::vec3{0.0f, 1.0f, 0.0f};
            geometry.capsuleHalfHeight = collider.capsuleHalfHeight * scale.y;

            if (collider.type == ColliderComponent::Type::Sphere) {
                geometry.halfExtents = glm::vec3{geometry.radius};
                geometry.boundsHalf = glm::vec3{geometry.radius};
                geometry.boundsCenter = geometry.center;
            } else if (collider.type == ColliderComponent::Type::Capsule) {
                const glm::vec3 axisEnd = geometry.capsuleAxis * geometry.capsuleHalfHeight;
                geometry.boundsHalf = glm::abs(axisEnd) + glm::vec3{geometry.radius};
                geometry.boundsCenter = geometry.center;
                geometry.halfExtents = geometry.boundsHalf;
            } else if (collider.type == ColliderComponent::Type::ConvexHull) {
                geometry.pointCount = std::min(collider.hullPointCount,
                                               static_cast<uint8_t>(collider.hullPoints.size()));
                glm::vec3 minimum{1e30f};
                glm::vec3 maximum{-1e30f};
                glm::vec3 localMinimum{1e30f};
                glm::vec3 localMaximum{-1e30f};
                for (uint8_t pointIndex = 0; pointIndex < geometry.pointCount; ++pointIndex) {
                    geometry.points[pointIndex] = geometry.center +
                                                  geometry.axes * (collider.hullPoints[pointIndex] * scale);
                    minimum = glm::min(minimum, geometry.points[pointIndex]);
                    maximum = glm::max(maximum, geometry.points[pointIndex]);
                    localMinimum = glm::min(localMinimum, collider.hullPoints[pointIndex] * scale);
                    localMaximum = glm::max(localMaximum, collider.hullPoints[pointIndex] * scale);
                }
                geometry.boundsCenter = (minimum + maximum) * 0.5f;
                geometry.boundsHalf = (maximum - minimum) * 0.5f;
                geometry.halfExtents = (localMaximum - localMinimum) * 0.5f;
            } else {
                const glm::vec3 orientedHalf = geometry.halfExtents;
                geometry.boundsHalf =
                    glm::vec3{glm::dot(glm::abs(glm::vec3{geometry.axes[0].x, geometry.axes[1].x, geometry.axes[2].x}),
                                        orientedHalf),
                              glm::dot(glm::abs(glm::vec3{geometry.axes[0].y, geometry.axes[1].y, geometry.axes[2].y}),
                                        orientedHalf),
                              glm::dot(glm::abs(glm::vec3{geometry.axes[0].z, geometry.axes[1].z, geometry.axes[2].z}),
                                        orientedHalf)};
                geometry.boundsCenter = geometry.center;
            }
        }
    };

    if (jobs != nullptr && m_bodies.size() >= parallelMinBodies) {
        jobs->parallelFor(m_bodies.size(), 0, buildRange);
    } else {
        buildRange(0, m_bodies.size());
    }
}

void PhysicsSystem::broadPhase() {
    m_planeBodies.clear();
    m_candidatePairs.clear();
    m_broadBodyIndex.clear();

    for (size_t i = 0; i < m_bodies.size(); ++i) {
        const uint32_t index = static_cast<uint32_t>(i);
        if (m_geometry[i].isPlane) {
            m_planeBodies.push_back(index);
            continue;
        }
        const BodyGeometry& geometry = m_geometry[i];
        const uint32_t key = entt::to_integral(m_bodies[i].entity);
        m_broadBodyIndex.emplace(key, index);
        // Sync the persistent tree with this step's tight world box.
        // The fat box absorbs small motion, so steady scenes only pay
        // for moveLeaf when a body genuinely travels.
        const AABB tight{geometry.boundsCenter - geometry.boundsHalf,
                         geometry.boundsCenter + geometry.boundsHalf};
        if (const auto found = m_broadProxies.find(key); found != m_broadProxies.end()) {
            m_broadTree.moveLeaf(found->second, tight, geometry.displacement);
        } else {
            m_broadProxies.emplace(key, m_broadTree.insertLeaf(tight, m_bodies[i].entity));
        }
    }

    // Entities that left the simulation (destroyed, or collider
    // changed to a plane) lose their proxies.
    for (auto it = m_broadProxies.begin(); it != m_broadProxies.end();) {
        if (m_broadBodyIndex.find(it->first) == m_broadBodyIndex.end()) {
            m_broadTree.removeLeaf(it->second);
            it = m_broadProxies.erase(it);
        } else {
            ++it;
        }
    }

    // Query the tree once per body with the swept box, then keep the
    // canonical half of each discovered pair. A candidate pair exists
    // exactly when one body's swept trajectory overlaps the other's
    // fat box (a superset of its tight box), so every pair that can
    // touch or be crossed this step is found; compared to the old
    // X-axis sweep this only drops pairs that were separated in Y or
    // Z all along — false candidates the narrow phase used to reject.
    for (size_t i = 0; i < m_bodies.size(); ++i) {
        const uint32_t index = static_cast<uint32_t>(i);
        const BodyGeometry& geometry = m_geometry[i];
        if (geometry.isPlane) {
            continue;
        }
        // The swept query box: the end box unioned with itself shifted
        // back along this step's motion — the whole trajectory of the
        // body is a candidate region, matching the old sweep's pad.
        const AABB swept{geometry.boundsCenter - geometry.boundsHalf +
                             glm::min(-geometry.displacement, glm::vec3{0.0f}),
                         geometry.boundsCenter + geometry.boundsHalf +
                             glm::max(-geometry.displacement, glm::vec3{0.0f})};
        m_broadTree.query(swept, [this, index](entt::entity other) {
            const auto found = m_broadBodyIndex.find(entt::to_integral(other));
            // Layer/mask filtering happens here, in the broadphase:
            // a pair whose colliders do not mutually accept each
            // other's layer never becomes a candidate, so it costs
            // no manifold generation and no solver work at all.
            if (found != m_broadBodyIndex.end() && found->second > index &&
                shouldCollide(*m_bodies[index].collider, *m_bodies[found->second].collider)) {
                m_candidatePairs.emplace_back(index, found->second);
            }
            return true;
        });
    }

    // Planes have infinite extent: they pair against every body whose
    // layers mutually accept (the same broadphase filter), and are
    // never stored in the tree.
    for (const uint32_t planeBody : m_planeBodies) {
        for (size_t i = 0; i < m_bodies.size(); ++i) {
            const uint32_t other = static_cast<uint32_t>(i);
            if (other == planeBody || m_geometry[other].isPlane ||
                !shouldCollide(*m_bodies[planeBody].collider, *m_bodies[other].collider)) {
                continue;
            }
            m_candidatePairs.emplace_back(std::min(planeBody, other), std::max(planeBody, other));
        }
    }

    std::sort(m_candidatePairs.begin(), m_candidatePairs.end());
    m_candidatePairs.erase(std::unique(m_candidatePairs.begin(), m_candidatePairs.end()), m_candidatePairs.end());
}

void PhysicsSystem::narrowPhase(JobSystem* jobs, uint32_t parallelMinBodies) {
    m_contactSlots.assign(m_candidatePairs.size(), std::vector<Contact>{});
    // Trigger pairs (either collider is a trigger volume) produce
    // overlap events instead of contact response. They are marked
    // serially here so the parallel manifold pass below can skip
    // them — which also keeps CCD from stopping bodies at a
    // trigger's surface.
    m_triggerPairFlags.assign(m_candidatePairs.size(), 0);
    m_stepTriggerPairs.clear();
    for (size_t i = 0; i < m_candidatePairs.size(); ++i) {
        const auto [bodyA, bodyB] = m_candidatePairs[i];
        if (m_bodies[bodyA].collider->isTrigger || m_bodies[bodyB].collider->isTrigger) {
            m_triggerPairFlags[i] = 1;
            const uint32_t entityA = entt::to_integral(m_bodies[bodyA].entity);
            const uint32_t entityB = entt::to_integral(m_bodies[bodyB].entity);
            m_stepTriggerPairs.push_back((static_cast<uint64_t>(std::min(entityA, entityB)) << 32u) |
                                         static_cast<uint64_t>(std::max(entityA, entityB)));
        }
    }
    const auto evaluateRange = [this](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const auto [bodyA, bodyB] = m_candidatePairs[i];
            if (m_triggerPairFlags[i] != 0) {
                continue;
            }
            collideManifold(bodyA, bodyB, m_contactSlots[i]);
            if (m_contactSlots[i].empty() && m_ccdEnabled) {
                const glm::vec3 relativeDisplacement =
                    m_geometry[bodyB].displacement - m_geometry[bodyA].displacement;
                if (glm::length(relativeDisplacement) > 0.05f) {
                    Contact base;
                    base.bodyA = bodyA;
                    base.bodyB = bodyB;
                    base.restitution = combinedRestitution(*m_bodies[bodyA].body, *m_bodies[bodyB].body);
                    base.friction = combinedFriction(*m_bodies[bodyA].body, *m_bodies[bodyB].body);
                    base.anisotropicFriction =
                        combinedAnisotropicFriction(*m_bodies[bodyA].body, *m_bodies[bodyB].body);
                    const glm::vec3 centerDelta = m_geometry[bodyB].center - m_geometry[bodyA].center;
                    base.normal = glm::length(centerDelta) > kEpsilon ? glm::normalize(centerDelta)
                                                                      : glm::normalize(relativeDisplacement);
                    base.penetration = 0.001f;
                    if (const std::optional<Contact> swept = sweptCollide(bodyA, bodyB, base)) {
                        Contact contact = *swept;
                        contact.point = contactPointWorld(bodyA, bodyB, contact.normal);
                        m_contactSlots[i].push_back(contact);
                    }
                }
            }
        }
    };

    if (jobs != nullptr && m_candidatePairs.size() >= parallelMinBodies) {
        jobs->parallelFor(m_candidatePairs.size(), 0, evaluateRange);
    } else {
        evaluateRange(0, m_candidatePairs.size());
    }

    m_contacts.clear();
    for (const std::vector<Contact>& manifold : m_contactSlots) {
        for (const Contact& contact : manifold) {
            m_contacts.push_back(contact);
        }
    }

    // Tag every contact with its manifold size: positional correction is
    // split across the points of a pair so a 4-point face contact does
    // not correct four times as hard as a single-point contact.
    std::unordered_map<uint64_t, uint8_t> manifoldCounts;
    for (const Contact& contact : m_contacts) {
        const uint64_t key = (static_cast<uint64_t>(contact.bodyA) << 32u) | contact.bodyB;
        uint8_t& count = manifoldCounts[key];
        count = static_cast<uint8_t>(std::min(255, static_cast<int>(count) + 1));
    }
    for (Contact& contact : m_contacts) {
        const uint64_t key = (static_cast<uint64_t>(contact.bodyA) << 32u) | contact.bodyB;
        contact.manifoldCount = manifoldCounts[key];
    }

    for (Contact& contact : m_contacts) {
        contact.featurePoint = contact.point;
    }

    // Manifold centroid reduction for the velocity solve: every point of
    // a manifold shares the manifold's centroid as its lever arm. A face
    // contact's centroid sits under the center of mass, so resting and
    // sliding bodies get no spurious pitch/roll/yaw torque from
    // sequential per-corner impulses (the classic manifold ratchet),
    // while an edge/corner manifold's centroid is genuinely offset, so
    // real tipping moments still resolve through it. The centroid is
    // weighted by per-point penetration depth, so when a box leans its
    // loaded (deeper) corners pull the force line toward themselves —
    // the pressure-shift righting moment that lets offset stacks
    // settle instead of rocking forever. Falls back to the plain
    // centroid when every depth is zero (resting exactly at the slop).
    std::unordered_map<uint64_t, glm::vec3> centroidSums;
    std::unordered_map<uint64_t, glm::vec3> plainSums;
    std::unordered_map<uint64_t, float> centroidWeights;
    for (const Contact& contact : m_contacts) {
        const uint64_t key = (static_cast<uint64_t>(contact.bodyA) << 32u) | contact.bodyB;
        const float weight = std::max(contact.penetration, 0.0f);
        centroidSums[key] += contact.point * weight;
        plainSums[key] += contact.point;
        centroidWeights[key] += weight;
    }
    for (Contact& contact : m_contacts) {
        const uint64_t key = (static_cast<uint64_t>(contact.bodyA) << 32u) | contact.bodyB;
        const float totalWeight = centroidWeights[key];
        contact.point = totalWeight > kEpsilon
                            ? centroidSums[key] / totalWeight
                            : plainSums[key] / static_cast<float>(contact.manifoldCount);
    }
}

glm::vec3 PhysicsSystem::supportPointWorld(uint32_t bodyIndex, const glm::vec3& direction) const {
    const BodyGeometry& geometry = m_geometry[bodyIndex];
    const glm::vec3 dir = glm::dot(direction, direction) > kEpsilon ? glm::normalize(direction) : direction;
    switch (geometry.type) {
    case ColliderComponent::Type::Sphere:
        return geometry.center + dir * geometry.radius;
    case ColliderComponent::Type::Capsule: {
        const float endSign = glm::dot(geometry.capsuleAxis, dir) >= 0.0f ? 1.0f : -1.0f;
        return geometry.center + geometry.capsuleAxis * (geometry.capsuleHalfHeight * endSign) +
               dir * geometry.radius;
    }
    case ColliderComponent::Type::ConvexHull: {
        glm::vec3 best = geometry.center;
        float bestDot = -1e30f;
        for (uint8_t i = 0; i < geometry.pointCount; ++i) {
            const float candidate = glm::dot(geometry.points[i], dir);
            if (candidate > bestDot) {
                bestDot = candidate;
                best = geometry.points[i];
            }
        }
        return best;
    }
    case ColliderComponent::Type::Box:
    case ColliderComponent::Type::Obb:
    case ColliderComponent::Type::Plane:
        break;
    }
    // Box-like: the corner furthest along the direction.
    glm::vec3 point = geometry.center;
    for (int axis = 0; axis < 3; ++axis) {
        const float sign = glm::dot(geometry.axes[axis], dir) >= 0.0f ? 1.0f : -1.0f;
        point += geometry.axes[axis] * (sign * geometry.halfExtents[axis]);
    }
    return point;
}

glm::vec3 PhysicsSystem::contactPointWorld(uint32_t bodyA, uint32_t bodyB, const glm::vec3& normal) const {
    const BodyGeometry& a = m_geometry[bodyA];
    const BodyGeometry& b = m_geometry[bodyB];
    const glm::vec3 n = glm::dot(normal, normal) > kEpsilon ? glm::normalize(normal) : normal;
    // Depth position: halfway between the two support faces along n.
    const glm::vec3 supportA = supportPointWorld(bodyA, n);
    const glm::vec3 supportB = supportPointWorld(bodyB, -n);
    const float faceMidDepth = 0.5f * (glm::dot(supportA, n) + glm::dot(supportB, n));
    // Tangential position: between the two centers (support corners of
    // a large body would fabricate lever arms meters long).
    const glm::vec3 centerMid = (a.center + b.center) * 0.5f;
    const glm::vec3 tangential = centerMid - n * glm::dot(centerMid, n);
    // Capsules are the exception: their contact rides the segment, so
    // use the segment point closest to the other center.
    const auto capsulePoint = [](const BodyGeometry& capsule, const glm::vec3& otherCenter,
                                 const glm::vec3& dir) {
        const glm::vec3 capA = capsule.center - capsule.capsuleAxis * capsule.capsuleHalfHeight;
        const glm::vec3 capB = capsule.center + capsule.capsuleAxis * capsule.capsuleHalfHeight;
        const glm::vec3 segment = capB - capA;
        const float lengthSq = glm::dot(segment, segment);
        const float t = lengthSq > kEpsilon
                            ? glm::clamp(glm::dot(otherCenter - capA, segment) / lengthSq, 0.0f, 1.0f)
                            : 0.0f;
        return capA + segment * t + dir * capsule.radius;
    };
    if (a.type == ColliderComponent::Type::Capsule) {
        const glm::vec3 onCapsule = capsulePoint(a, b.center, n);
        return onCapsule - n * (0.5f * glm::dot(onCapsule - supportB, n));
    }
    if (b.type == ColliderComponent::Type::Capsule) {
        const glm::vec3 onCapsule = capsulePoint(b, a.center, -n);
        return onCapsule + n * (0.5f * glm::dot(supportA - onCapsule, n));
    }
    return tangential + n * faceMidDepth;
}

void PhysicsSystem::collideManifold(uint32_t bodyA, uint32_t bodyB, std::vector<Contact>& out) const {
    out.clear();
    const BodyGeometry& a = m_geometry[bodyA];
    const BodyGeometry& b = m_geometry[bodyB];
    const auto isBoxLike = [](const BodyGeometry& geometry) {
        return geometry.type == ColliderComponent::Type::Box ||
               geometry.type == ColliderComponent::Type::Obb;
    };

    const auto boxCorners = [](const BodyGeometry& geometry, std::array<glm::vec3, 8>& corners) {
        size_t cornerIndex = 0;
        for (const float sx : {-1.0f, 1.0f}) {
            for (const float sy : {-1.0f, 1.0f}) {
                for (const float sz : {-1.0f, 1.0f}) {
                    corners[cornerIndex++] = geometry.center + geometry.axes[0] * (sx * geometry.halfExtents.x) +
                                             geometry.axes[1] * (sy * geometry.halfExtents.y) +
                                             geometry.axes[2] * (sz * geometry.halfExtents.z);
                }
            }
        }
    };

    // Dedicated manifold: box resting on a plane — every box corner below
    // the plane becomes a contact with its own penetration depth, so a
    // landed box is supported across its face, not balanced on a point.
    if ((a.isPlane || b.isPlane) && !(a.isPlane && b.isPlane)) {
        const uint32_t planeIndex = a.isPlane ? bodyA : bodyB;
        const uint32_t otherIndex = a.isPlane ? bodyB : bodyA;
        const BodyGeometry& plane = m_geometry[planeIndex];
        const BodyGeometry& other = m_geometry[otherIndex];
        if (isBoxLike(other)) {
            const float offset = m_bodies[planeIndex].collider->planeOffset;
            std::array<glm::vec3, 8> corners{};
            boxCorners(other, corners);
            std::vector<Contact> points;
            for (const glm::vec3& corner : corners) {
                const float penetration = -(glm::dot(plane.planeNormal, corner) - offset);
                if (penetration > 0.0f) {
                    Contact contact;
                    contact.bodyA = bodyA;
                    contact.bodyB = bodyB;
                    contact.normal = planeIndex == bodyA ? plane.planeNormal : -plane.planeNormal;
                    contact.point = corner;
                    contact.penetration = penetration;
                    contact.restitution = combinedRestitution(*m_bodies[bodyA].body, *m_bodies[bodyB].body);
                    contact.friction = combinedFriction(*m_bodies[bodyA].body, *m_bodies[bodyB].body);
                    contact.anisotropicFriction =
                        combinedAnisotropicFriction(*m_bodies[bodyA].body, *m_bodies[bodyB].body);
                    points.push_back(contact);
                }
            }
            if (!points.empty()) {
                // Deepest first, then farthest-point reduction to at
                // most four: a face contact keeps its four corners
                // spread across the face (never clustered on one edge).
                std::sort(points.begin(), points.end(), [](const Contact& lhs, const Contact& rhs) {
                    return lhs.penetration > rhs.penetration;
                });
                if (points.size() > 4) {
                    std::vector<Contact> reduced;
                    reduced.push_back(points.front());
                    while (reduced.size() < 4) {
                        float bestDistance = -1.0f;
                        size_t bestIndex = 0;
                        for (size_t i = 0; i < points.size(); ++i) {
                            bool alreadyPicked = false;
                            for (const Contact& picked : reduced) {
                                alreadyPicked = alreadyPicked || picked.point == points[i].point;
                            }
                            if (alreadyPicked) {
                                continue;
                            }
                            float minDistance = 1e30f;
                            for (const Contact& picked : reduced) {
                                minDistance = std::min(minDistance,
                                                       glm::dot(picked.point - points[i].point,
                                                                picked.point - points[i].point));
                            }
                            if (minDistance > bestDistance) {
                                bestDistance = minDistance;
                                bestIndex = i;
                            }
                        }
                        reduced.push_back(points[bestIndex]);
                    }
                    points = std::move(reduced);
                }
                out = std::move(points);
                return;
            }
        }
    }

    // Dedicated manifold: box vs box — corners of either box that lie
    // inside the other become contact points along the SAT normal. A
    // resting face contact yields the four bottom corners; an edge/corner
    // contact yields one or two, so tipping moments resolve through real
    // lever arms instead of a single central push.
    if (isBoxLike(a) && isBoxLike(b)) {
        const std::optional<Contact> base = collide(bodyA, bodyB);
        if (!base.has_value()) {
            return;
        }
        const auto cornersInside = [&boxCorners](const BodyGeometry& source, const BodyGeometry& container,
                                                 const glm::vec3& worldNormal, float penetration,
                                                 std::vector<glm::vec3>& inside) {
            std::array<glm::vec3, 8> corners{};
            boxCorners(source, corners);
            const glm::mat3 worldToLocal = glm::transpose(container.axes);
            const glm::vec3 tolerance{0.005f};
            for (const glm::vec3& corner : corners) {
                const glm::vec3 local = worldToLocal * (corner - container.center);
                const glm::vec3 absLocal = glm::abs(local);
                if (absLocal.x <= container.halfExtents.x + tolerance.x &&
                    absLocal.y <= container.halfExtents.y + tolerance.y &&
                    absLocal.z <= container.halfExtents.z + tolerance.z) {
                    inside.push_back(corner);
                    continue;
                }
                // Passed-through feature: a corner that crossed a
                // thin body overshoots it along the contact normal
                // only. Overshoot in any other direction means the
                // corner is simply outside (an edge contact keeps
                // exactly its two true endpoints).
                const glm::vec3 excessLocal = glm::sign(local) * glm::max(absLocal - container.halfExtents, glm::vec3{0.0f});
                const glm::vec3 excessWorld = container.axes * excessLocal;
                const float excessLength = glm::length(excessWorld);
                if (excessLength > kEpsilon && excessLength <= penetration + 0.005f &&
                    std::abs(glm::dot(excessWorld / excessLength, worldNormal)) > 0.9f) {
                    inside.push_back(corner);
                }
            }
        };
        // Each manifold point also gets its own penetration depth
        // along the normal: how far that corner has crossed the other
        // box's near face. A leaning box's deep corner carries a
        // larger depth than its lifted corner — the manifold centroid
        // below is weighted by these depths, so the solver's force
        // line shifts onto the loaded edge and rights the box, the
        // way real contact pressure redistributes.
        const glm::vec3 supportA = supportPointWorld(bodyA, base->normal);
        const glm::vec3 supportB = supportPointWorld(bodyB, -base->normal);
        std::vector<glm::vec3> points;
        std::vector<float> pointPenetrations;
        const auto collectWithDepth = [&](uint32_t source, uint32_t container, const glm::vec3& farSupport) {
            std::vector<glm::vec3> inside;
            cornersInside(m_geometry[source], m_geometry[container], base->normal, base->penetration, inside);
            for (const glm::vec3& point : inside) {
                points.push_back(point);
                const float depth = source == bodyA ? glm::dot(point - farSupport, base->normal)
                                                    : glm::dot(farSupport - point, base->normal);
                pointPenetrations.push_back(std::max(depth, 0.0f));
            }
        };
        collectWithDepth(bodyA, bodyB, supportB);
        collectWithDepth(bodyB, bodyA, supportA);

        if (points.size() >= 2) {
            // Sort/de-duplicate/reduce points with their depths attached.
            std::vector<size_t> order(points.size());
            for (size_t i = 0; i < order.size(); ++i) {
                order[i] = i;
            }
            std::sort(order.begin(), order.end(), [&points](size_t lhs, size_t rhs) {
                if (points[lhs].x != points[rhs].x) {
                    return points[lhs].x < points[rhs].x;
                }
                if (points[lhs].y != points[rhs].y) {
                    return points[lhs].y < points[rhs].y;
                }
                return points[lhs].z < points[rhs].z;
            });
            std::vector<glm::vec3> sortedPoints;
            std::vector<float> sortedPenetrations;
            for (const size_t index : order) {
                if (!sortedPoints.empty() &&
                    glm::dot(sortedPoints.back() - points[index], sortedPoints.back() - points[index]) < 1e-8f) {
                    sortedPenetrations.back() = std::max(sortedPenetrations.back(), pointPenetrations[index]);
                    continue;
                }
                sortedPoints.push_back(points[index]);
                sortedPenetrations.push_back(pointPenetrations[index]);
            }
            points = std::move(sortedPoints);
            pointPenetrations = std::move(sortedPenetrations);
            if (points.size() > 4) {
                // Farthest-point reduction: keep four points spread
                // across the contact face, never clustered on one edge
                // (a clustered manifold tips the resting body over).
                std::vector<size_t> reduced{0};
                while (reduced.size() < 4) {
                    float bestDistance = -1.0f;
                    size_t bestIndex = 0;
                    for (size_t i = 0; i < points.size(); ++i) {
                        bool alreadyPicked = false;
                        for (const size_t picked : reduced) {
                            alreadyPicked = alreadyPicked || picked == i;
                        }
                        if (alreadyPicked) {
                            continue;
                        }
                        float minDistance = 1e30f;
                        for (const size_t picked : reduced) {
                            minDistance = std::min(minDistance,
                                                   glm::dot(points[picked] - points[i], points[picked] - points[i]));
                        }
                        if (minDistance > bestDistance) {
                            bestDistance = minDistance;
                            bestIndex = i;
                        }
                    }
                    reduced.push_back(bestIndex);
                }
                std::vector<glm::vec3> reducedPoints;
                std::vector<float> reducedPenetrations;
                for (const size_t index : reduced) {
                    reducedPoints.push_back(points[index]);
                    reducedPenetrations.push_back(pointPenetrations[index]);
                }
                points = std::move(reducedPoints);
                pointPenetrations = std::move(reducedPenetrations);
            }
            for (size_t i = 0; i < points.size(); ++i) {
                Contact contact = *base;
                contact.point = points[i];
                contact.penetration = pointPenetrations[i];
                out.push_back(contact);
            }
            return;
        }
        Contact contact = *base;
        contact.point = contactPointWorld(bodyA, bodyB, contact.normal);
        out.push_back(contact);
        return;
    }

    // Dedicated manifold: capsule vs box — both capsule segment endpoints
    // are tested against the box, so a capsule lying/standing across a
    // box top (the player!) is supported at two points instead of one.
    const bool aIsCapsule = a.type == ColliderComponent::Type::Capsule;
    const bool bIsCapsule = b.type == ColliderComponent::Type::Capsule;
    if ((aIsCapsule && isBoxLike(b)) || (bIsCapsule && isBoxLike(a))) {
        const std::optional<Contact> base = collide(bodyA, bodyB);
        if (!base.has_value()) {
            return;
        }
        const uint32_t capsuleIndex = aIsCapsule ? bodyA : bodyB;
        const uint32_t boxIndex = aIsCapsule ? bodyB : bodyA;
        const BodyGeometry& capsule = m_geometry[capsuleIndex];
        const BodyGeometry& box = m_geometry[boxIndex];
        const glm::mat3 worldToLocal = glm::transpose(box.axes);
        {
            // Primary point: the true closest pair between the capsule
            // segment and the box surface (mid-segment features
            // included), not just an endpoint.
            const glm::vec3 localA = worldToLocal * (capsule.center - capsule.capsuleAxis * capsule.capsuleHalfHeight - box.center);
            const glm::vec3 localB = worldToLocal * (capsule.center + capsule.capsuleAxis * capsule.capsuleHalfHeight - box.center);
            float closestT = 0.0f;
            glm::vec3 segmentPoint;
            glm::vec3 boxPoint;
            const float distance = segmentBoxDistance(localA, localB, box.halfExtents, closestT, segmentPoint, boxPoint);
            if (distance < capsule.radius + 0.01f) {
                Contact contact = *base;
                contact.point = box.center + box.axes * ((segmentPoint + boxPoint) * 0.5f);
                contact.penetration =
                    distance > kEpsilon ? std::max(capsule.radius - distance, 0.0005f) : base->penetration;
                out.push_back(contact);
            }
        }
        for (const float endSign : {-1.0f, 1.0f}) {
            const glm::vec3 endpoint = capsule.center + capsule.capsuleAxis * (capsule.capsuleHalfHeight * endSign);
            const glm::vec3 local = worldToLocal * (endpoint - box.center);
            const glm::vec3 clamped = glm::clamp(local, -box.halfExtents, box.halfExtents);
            const glm::vec3 closest = box.center + box.axes * clamped;
            const float distance = glm::length(endpoint - closest);
            if (distance < capsule.radius + 0.01f) {
                Contact contact = *base;
                contact.point = (endpoint + closest) * 0.5f;
                contact.penetration =
                    distance > kEpsilon ? std::max(capsule.radius - distance, 0.0005f) : base->penetration;
                bool duplicate = false;
                for (const Contact& existing : out) {
                    duplicate = duplicate || glm::dot(existing.point - contact.point,
                                                      existing.point - contact.point) < 0.0016f;
                }
                if (!duplicate) {
                    out.push_back(contact);
                }
            }
        }
        if (out.empty()) {
            Contact contact = *base;
            contact.point = contactPointWorld(bodyA, bodyB, contact.normal);
            out.push_back(contact);
        }
        return;
    }

    // Generic path: the single-point solver contact, with its lever arm
    // at the midpoint of the two support points along the normal.
    if (const std::optional<Contact> contact = collide(bodyA, bodyB)) {
        Contact result = *contact;
        result.point = contactPointWorld(bodyA, bodyB, result.normal);
        out.push_back(result);
    }
}

std::optional<PhysicsSystem::Contact> PhysicsSystem::collide(uint32_t bodyA, uint32_t bodyB) const {
    if (inverseMass(bodyA) == 0.0f && inverseMass(bodyB) == 0.0f) {
        return std::nullopt;
    }

    const BodyGeometry& a = m_geometry[bodyA];
    const BodyGeometry& b = m_geometry[bodyB];
    const RigidBodyComponent& bodyDataA = *m_bodies[bodyA].body;
    const RigidBodyComponent& bodyDataB = *m_bodies[bodyB].body;

    Contact contact;
    contact.bodyA = bodyA;
    contact.bodyB = bodyB;
    contact.restitution = combinedRestitution(bodyDataA, bodyDataB);
    contact.friction = combinedFriction(bodyDataA, bodyDataB);
    contact.anisotropicFriction = combinedAnisotropicFriction(bodyDataA, bodyDataB);

    const auto finish = [this, &contact, bodyA, bodyB](std::optional<Contact> result) {
        if (!result.has_value()) {
            return result;
        }
        if (!m_ccdEnabled) {
            return result;
        }
        const glm::vec3 relativeDisplacement = m_geometry[bodyB].displacement - m_geometry[bodyA].displacement;
        const float displacementLength = glm::length(relativeDisplacement);
        const float thickness = std::max(0.05f, std::min(m_geometry[bodyA].radius, m_geometry[bodyB].radius));
        if (displacementLength <= thickness * 0.5f) {
            return result;
        }
        if (const std::optional<Contact> swept = sweptCollide(bodyA, bodyB, *result); swept.has_value()) {
            return swept;
        }
        return result;
    };

    if (a.isPlane && b.isPlane) {
        return std::nullopt;
    }

    if (a.isPlane || b.isPlane) {
        const uint32_t planeIndex = a.isPlane ? bodyA : bodyB;
        const uint32_t otherIndex = a.isPlane ? bodyB : bodyA;
        const BodyGeometry& plane = m_geometry[planeIndex];
        const BodyGeometry& other = m_geometry[otherIndex];
        const float offset = m_bodies[planeIndex].collider->planeOffset;
        const float signedDistance = glm::dot(plane.planeNormal, other.center) - offset;
        float supportRadius = other.boundsHalf.y;
        if (other.type == ColliderComponent::Type::Sphere) {
            supportRadius = other.radius;
        } else if (other.type == ColliderComponent::Type::Capsule) {
            supportRadius = glm::dot(glm::abs(plane.planeNormal), glm::abs(other.capsuleAxis) * other.capsuleHalfHeight) +
                            other.radius;
        } else if (other.type == ColliderComponent::Type::ConvexHull && other.pointCount > 0) {
            float minimum = 1e30f;
            for (uint8_t pointIndex = 0; pointIndex < other.pointCount; ++pointIndex) {
                minimum = std::min(minimum, glm::dot(plane.planeNormal, other.points[pointIndex]) - offset);
            }
            if (minimum > 0.0f) {
                return std::nullopt;
            }
            contact.normal = (planeIndex == bodyA) ? plane.planeNormal : -plane.planeNormal;
            contact.penetration = -minimum;
            return finish(contact);
        } else {
            supportRadius = std::abs(glm::dot(plane.planeNormal, other.axes[0])) * other.halfExtents.x +
                            std::abs(glm::dot(plane.planeNormal, other.axes[1])) * other.halfExtents.y +
                            std::abs(glm::dot(plane.planeNormal, other.axes[2])) * other.halfExtents.z;
        }
        if (other.type != ColliderComponent::Type::ConvexHull) {
            const float penetration = static_cast<float>(supportRadius) - signedDistance;
            if (penetration <= 0.0f) {
                return std::nullopt;
            }
            contact.normal = (planeIndex == bodyA) ? plane.planeNormal : -plane.planeNormal;
            contact.penetration = penetration;
        }
        return finish(contact);
    }

    const auto typeOf = [this](uint32_t index) { return m_bodies[index].collider->type; };
    const bool aIsSphere = typeOf(bodyA) == ColliderComponent::Type::Sphere;
    const bool bIsSphere = typeOf(bodyB) == ColliderComponent::Type::Sphere;
    const bool aIsCapsule = typeOf(bodyA) == ColliderComponent::Type::Capsule;
    const bool bIsCapsule = typeOf(bodyB) == ColliderComponent::Type::Capsule;
    const bool aIsBoxLike = typeOf(bodyA) == ColliderComponent::Type::Box || typeOf(bodyA) == ColliderComponent::Type::Obb;
    const bool bIsBoxLike = typeOf(bodyB) == ColliderComponent::Type::Box || typeOf(bodyB) == ColliderComponent::Type::Obb;
    const auto supportShape = [](ColliderComponent::Type type) {
        return type == ColliderComponent::Type::Box || type == ColliderComponent::Type::Obb ||
               type == ColliderComponent::Type::Sphere || type == ColliderComponent::Type::Capsule ||
               type == ColliderComponent::Type::ConvexHull;
    };

    if (aIsSphere && bIsSphere) {
        const glm::vec3 delta = b.center - a.center;
        const float radiusSum = a.radius + b.radius;
        const float distanceSq = glm::dot(delta, delta);
        if (distanceSq >= radiusSum * radiusSum) {
            return std::nullopt;
        }
        const float distance = std::sqrt(distanceSq);
        contact.normal = distance > kEpsilon ? delta / distance : glm::vec3{1.0f, 0.0f, 0.0f};
        contact.penetration = radiusSum - distance;
        return finish(contact);
    }

    if ((aIsCapsule || bIsCapsule) && supportShape(typeOf(bodyA)) && supportShape(typeOf(bodyB))) {
        const uint32_t capsuleIndex = aIsCapsule ? bodyA : bodyB;
        const uint32_t otherIndex = aIsCapsule ? bodyB : bodyA;
        const BodyGeometry& capsule = m_geometry[capsuleIndex];
        const BodyGeometry& other = m_geometry[otherIndex];
        const glm::vec3 capA = capsule.center - capsule.capsuleAxis * capsule.capsuleHalfHeight;
        const glm::vec3 capB = capsule.center + capsule.capsuleAxis * capsule.capsuleHalfHeight;
        glm::vec3 closestOnCapsule = capA;
        if (other.type == ColliderComponent::Type::Sphere) {
            const glm::vec3 segment = capB - capA;
            const float lengthSq = glm::dot(segment, segment);
            const float t = lengthSq > kEpsilon ? glm::clamp(glm::dot(other.center - capA, segment) / lengthSq, 0.0f, 1.0f) : 0.0f;
            closestOnCapsule = capA + segment * t;
            const glm::vec3 delta = other.center - closestOnCapsule;
            const float distance = glm::length(delta);
            const float radiusSum = other.radius + capsule.radius;
            if (distance >= radiusSum) {
                return std::nullopt;
            }
            contact.normal = (capsuleIndex == bodyA ? -1.0f : 1.0f) * (distance > kEpsilon ? delta / distance : glm::vec3{1.0f, 0.0f, 0.0f});
            contact.penetration = radiusSum - distance;
            return finish(contact);
        }

        if (other.type == ColliderComponent::Type::Capsule) {
            const glm::vec3 otherA = other.center - other.capsuleAxis * other.capsuleHalfHeight;
            const glm::vec3 otherB = other.center + other.capsuleAxis * other.capsuleHalfHeight;
            glm::vec3 closestCapsule;
            glm::vec3 closestOther;
            closestSegmentSegment(capA, capB, otherA, otherB, closestCapsule, closestOther);
            const glm::vec3 delta = closestOther - closestCapsule;
            const float distance = glm::length(delta);
            const float radiusSum = capsule.radius + other.radius;
            if (distance >= radiusSum) {
                return std::nullopt;
            }
            const glm::vec3 capsuleToOther =
                distance > kEpsilon ? delta / distance : glm::vec3{1.0f, 0.0f, 0.0f};
            contact.normal = capsuleIndex == bodyA ? capsuleToOther : -capsuleToOther;
            contact.penetration = radiusSum - distance;
            return finish(contact);
        }

        if (other.type == ColliderComponent::Type::Box || other.type == ColliderComponent::Type::Obb) {
            // Exact capsule/OBB test: transform the capsule segment into
            // box-local space and measure segment-to-AABB distance. This is
            // the player's 1:1 hitbox path — contact begins precisely when
            // the capsule surface reaches a face, edge, or corner.
            const glm::mat3 worldToLocal = glm::transpose(other.axes);
            const glm::vec3 localA = worldToLocal * (capA - other.center);
            const glm::vec3 localB = worldToLocal * (capB - other.center);
            float closestT = 0.0f;
            glm::vec3 segmentPoint;
            glm::vec3 boxPoint;
            const float distance = segmentBoxDistance(localA, localB, other.halfExtents, closestT, segmentPoint, boxPoint);
            glm::vec3 boxToCapsuleLocal;
            if (distance > kEpsilon) {
                if (distance >= capsule.radius) {
                    return std::nullopt;
                }
                boxToCapsuleLocal = (segmentPoint - boxPoint) / distance;
                contact.penetration = capsule.radius - distance;
            } else {
                // Segment passes through the box: leave through the nearest
                // face from the closest segment point.
                const glm::vec3 faceDistance = other.halfExtents - glm::abs(segmentPoint);
                int axis = 0;
                if (faceDistance.y < faceDistance[axis]) {
                    axis = 1;
                }
                if (faceDistance.z < faceDistance[axis]) {
                    axis = 2;
                }
                boxToCapsuleLocal = glm::vec3{0.0f};
                boxToCapsuleLocal[axis] = segmentPoint[axis] >= 0.0f ? 1.0f : -1.0f;
                contact.penetration = capsule.radius + faceDistance[axis];
            }
            const glm::vec3 boxToCapsule = glm::normalize(other.axes * boxToCapsuleLocal);
            contact.normal = capsuleIndex == bodyA ? -boxToCapsule : boxToCapsule;
            return finish(contact);
        }

        const glm::vec3 deltaMin = glm::abs(other.center - capsule.center) - other.boundsHalf;
        const float outsideDistance = glm::length(glm::max(deltaMin, glm::vec3{0.0f}));
        if (outsideDistance >= capsule.radius + std::max(other.boundsHalf.x, std::max(other.boundsHalf.y, other.boundsHalf.z))) {
            return std::nullopt;
        }
        const glm::vec3 delta = other.center - capsule.center;
        const glm::vec3 normal = glm::length(delta) > kEpsilon ? glm::normalize(delta) : glm::vec3{1.0f, 0.0f, 0.0f};
        contact.normal = capsuleIndex == bodyA ? normal : -normal;
        contact.penetration = capsule.radius + std::min(other.boundsHalf.x, std::min(other.boundsHalf.y, other.boundsHalf.z)) - glm::length(delta);
        if (contact.penetration <= 0.0f) {
            return std::nullopt;
        }
        return finish(contact);
    }

    if (aIsBoxLike && bIsBoxLike) {
        const auto orientedBounds = [](const BodyGeometry& geometry, const glm::vec3& axis) {
            return glm::dot(glm::abs(glm::vec3{glm::dot(axis, geometry.axes[0]), glm::dot(axis, geometry.axes[1]), glm::dot(axis, geometry.axes[2])}), geometry.halfExtents);
        };
        const std::array<glm::vec3, 6> axes = {a.axes[0], a.axes[1], a.axes[2], b.axes[0], b.axes[1], b.axes[2]};
        const glm::vec3 delta = b.center - a.center;
        float bestPenetration = 1e30f;
        glm::vec3 bestAxis{1.0f, 0.0f, 0.0f};
        for (const glm::vec3& rawAxis : axes) {
            const glm::vec3 axis = glm::dot(rawAxis, rawAxis) > kEpsilon ? glm::normalize(rawAxis) : glm::vec3{1.0f, 0.0f, 0.0f};
            const float separation = std::abs(glm::dot(delta, axis));
            const float projection = orientedBounds(a, axis) + orientedBounds(b, axis);
            const float penetration = projection - separation;
            if (penetration <= 0.0f) {
                return std::nullopt;
            }
            if (penetration < bestPenetration) {
                bestPenetration = penetration;
                bestAxis = glm::dot(delta, axis) < 0.0f ? -axis : axis;
            }
        }
        contact.normal = bestAxis;
        contact.penetration = bestPenetration;
        return finish(contact);
    }

    if ((aIsBoxLike || a.type == ColliderComponent::Type::ConvexHull) &&
        (bIsBoxLike || b.type == ColliderComponent::Type::ConvexHull) && !aIsSphere && !bIsSphere) {
        const glm::vec3 delta = b.center - a.center;
        const glm::vec3 overlap = (a.boundsHalf + b.boundsHalf) - glm::abs(delta - (b.boundsCenter - b.center) + (a.boundsCenter - a.center));
        const glm::vec3 centerDelta = b.boundsCenter - a.boundsCenter;
        const glm::vec3 boundsOverlap = (a.boundsHalf + b.boundsHalf) - glm::abs(centerDelta);
        if (boundsOverlap.x <= 0.0f || boundsOverlap.y <= 0.0f || boundsOverlap.z <= 0.0f) {
            return std::nullopt;
        }
        (void)overlap;
        if (boundsOverlap.x <= boundsOverlap.y && boundsOverlap.x <= boundsOverlap.z) {
            contact.normal = glm::vec3{centerDelta.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
            contact.penetration = boundsOverlap.x;
        } else if (boundsOverlap.y <= boundsOverlap.z) {
            contact.normal = glm::vec3{0.0f, centerDelta.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
            contact.penetration = boundsOverlap.y;
        } else {
            contact.normal = glm::vec3{0.0f, 0.0f, centerDelta.z >= 0.0f ? 1.0f : -1.0f};
            contact.penetration = boundsOverlap.z;
        }
        return finish(contact);
    }

    if (aIsSphere || bIsSphere) {
        const uint32_t sphereIndex = aIsSphere ? bodyA : bodyB;
        const uint32_t otherIndex = aIsSphere ? bodyB : bodyA;
        const BodyGeometry& sphere = m_geometry[sphereIndex];
        const BodyGeometry& other = m_geometry[otherIndex];
        const float support = glm::length(other.boundsHalf);
        const glm::vec3 delta = sphere.center - other.boundsCenter;
        const float distance = glm::length(delta);
        if (distance >= sphere.radius + support) {
            return std::nullopt;
        }
        glm::vec3 separationDirection = distance > kEpsilon ? delta / distance : glm::vec3{1.0f, 0.0f, 0.0f};
        float penetration = 0.0f;
        if (other.type == ColliderComponent::Type::Box || other.type == ColliderComponent::Type::Obb) {
            const glm::vec3 boxMin = other.center - other.boundsHalf;
            const glm::vec3 boxMax = other.center + other.boundsHalf;
            const glm::vec3 closest = glm::clamp(sphere.center, boxMin, boxMax);
            const glm::vec3 boxDelta = sphere.center - closest;
            const float boxDistance = glm::length(boxDelta);
            if (boxDistance > kEpsilon) {
                if (boxDistance >= sphere.radius) {
                    return std::nullopt;
                }
                separationDirection = boxDelta / boxDistance;
                penetration = sphere.radius - boxDistance;
            } else {
                penetration = sphere.radius + std::min({sphere.center.x - boxMin.x, boxMax.x - sphere.center.x,
                                                         sphere.center.y - boxMin.y, boxMax.y - sphere.center.y,
                                                         sphere.center.z - boxMin.z, boxMax.z - sphere.center.z});
            }
        } else if (other.type == ColliderComponent::Type::ConvexHull && other.pointCount > 0) {
            float minSq = 1e30f;
            glm::vec3 closestPoint = other.points[0];
            for (uint8_t pointIndex = 0; pointIndex < other.pointCount; ++pointIndex) {
                const glm::vec3 pointDelta = sphere.center - other.points[pointIndex];
                const float distSq = glm::dot(pointDelta, pointDelta);
                if (distSq < minSq) {
                    minSq = distSq;
                    closestPoint = other.points[pointIndex];
                }
            }
            const float closestDistance = std::sqrt(minSq);
            if (closestDistance >= sphere.radius) {
                return std::nullopt;
            }
            separationDirection = closestDistance > kEpsilon ? (sphere.center - closestPoint) / closestDistance : glm::vec3{1.0f, 0.0f, 0.0f};
            penetration = sphere.radius - closestDistance;
        } else {
            penetration = sphere.radius + std::min(other.boundsHalf.x, std::min(other.boundsHalf.y, other.boundsHalf.z)) - distance;
            if (penetration <= 0.0f) {
                return std::nullopt;
            }
        }
        contact.normal = aIsSphere ? -separationDirection : separationDirection;
        contact.penetration = penetration;
        return finish(contact);
    }

    const glm::vec3 delta = b.boundsCenter - a.boundsCenter;
    const glm::vec3 overlap = (a.boundsHalf + b.boundsHalf) - glm::abs(delta);
    if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f) {
        return std::nullopt;
    }
    if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
        contact.normal = glm::vec3{delta.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
        contact.penetration = overlap.x;
    } else if (overlap.y <= overlap.z) {
        contact.normal = glm::vec3{0.0f, delta.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
        contact.penetration = overlap.y;
    } else {
        contact.normal = glm::vec3{0.0f, 0.0f, delta.z >= 0.0f ? 1.0f : -1.0f};
        contact.penetration = overlap.z;
    }
    return finish(contact);
}

std::optional<PhysicsSystem::Contact> PhysicsSystem::sweptCollide(uint32_t bodyA, uint32_t bodyB,
                                                                  const Contact& base) const {
    const BodyGeometry& a = m_geometry[bodyA];
    const BodyGeometry& b = m_geometry[bodyB];
    const glm::vec3 relativeStart = a.previousCenter - b.previousCenter;
    const glm::vec3 relativeVelocity = a.displacement - b.displacement;
    const float relativeSpeed = glm::length(relativeVelocity);
    if (relativeSpeed <= kEpsilon) {
        return std::nullopt;
    }

    const bool aIsPlane = a.isPlane;
    const bool bIsPlane = b.isPlane;
    if (aIsPlane || bIsPlane) {
        const uint32_t planeIndex = aIsPlane ? bodyA : bodyB;
        const uint32_t otherIndex = aIsPlane ? bodyB : bodyA;
        const BodyGeometry& plane = m_geometry[planeIndex];
        const BodyGeometry& other = m_geometry[otherIndex];
        const float offset = m_bodies[planeIndex].collider->planeOffset;
        const float support = other.type == ColliderComponent::Type::Sphere ? other.radius : other.boundsHalf.y;
        const float startDistance = glm::dot(plane.planeNormal, other.previousCenter) - offset - support;
        const float endDistance = glm::dot(plane.planeNormal, other.center) - offset - support;
        if (startDistance > 0.0f && endDistance <= 0.0f) {
            const float time = startDistance / std::max(kEpsilon, startDistance - endDistance);
            Contact swept = base;
            swept.continuous = true;
            swept.timeOfImpact = glm::clamp(time, 0.0f, 1.0f);
            swept.penetration = std::max(0.001f, -endDistance + 0.001f);
            return swept;
        }
        return std::nullopt;
    }

    const auto sphereGeometry = [this](uint32_t index, const BodyGeometry*& geometry) {
        geometry = &m_geometry[index];
        return m_bodies[index].collider->type == ColliderComponent::Type::Sphere;
    };
    const BodyGeometry* sphereA = nullptr;
    const BodyGeometry* sphereB = nullptr;
    const bool aSphere = sphereGeometry(bodyA, sphereA);
    const bool bSphere = sphereGeometry(bodyB, sphereB);
    if ((aSphere && !bSphere) || (bSphere && !aSphere)) {
        const uint32_t sphereIndex = aSphere ? bodyA : bodyB;
        const uint32_t otherIndex = aSphere ? bodyB : bodyA;
        const BodyGeometry& sphere = m_geometry[sphereIndex];
        const BodyGeometry& other = m_geometry[otherIndex];
        if (other.type == ColliderComponent::Type::Box || other.type == ColliderComponent::Type::Obb) {
            const glm::vec3 start = sphere.previousCenter - other.previousCenter;
            const glm::vec3 motion = sphere.displacement - other.displacement;
            const glm::vec3 expandedHalf = other.boundsHalf + glm::vec3{sphere.radius};
            float timeMin = 0.0f;
            float timeMax = 1.0f;
            glm::vec3 hitNormal{1.0f, 0.0f, 0.0f};
            bool hit = true;
            for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
                const float startAxis = start[axisIndex];
                const float motionAxis = motion[axisIndex];
                const float halfAxis = expandedHalf[axisIndex];
                if (std::abs(motionAxis) <= kEpsilon) {
                    if (std::abs(startAxis) > halfAxis) {
                        hit = false;
                        break;
                    }
                    continue;
                }
                float t1 = (-halfAxis - startAxis) / motionAxis;
                float t2 = (halfAxis - startAxis) / motionAxis;
                glm::vec3 axisNormal{0.0f};
                axisNormal[axisIndex] = motionAxis > 0.0f ? -1.0f : 1.0f;
                if (t1 > t2) {
                    std::swap(t1, t2);
                }
                if (t1 > timeMin) {
                    timeMin = t1;
                    hitNormal = axisNormal;
                }
                timeMax = std::min(timeMax, t2);
                if (timeMin > timeMax) {
                    hit = false;
                    break;
                }
            }
            if (hit && timeMin >= 0.0f && timeMin <= 1.0f) {
                Contact swept = base;
                swept.continuous = true;
                swept.timeOfImpact = timeMin;
                swept.normal = sphereIndex == bodyA ? -hitNormal : hitNormal;
                swept.penetration = 0.001f;
                return swept;
            }
            return std::nullopt;
        }
        const glm::vec3 start = sphere.previousCenter - other.previousCenter;
        const glm::vec3 motion = sphere.displacement - other.displacement;
        const float radius = sphere.radius + std::max(other.boundsHalf.x, std::max(other.boundsHalf.y, other.boundsHalf.z));
        const float aCoeff = glm::dot(motion, motion);
        if (aCoeff <= kEpsilon) {
            return std::nullopt;
        }
        const float bCoeff = 2.0f * glm::dot(start, motion);
        const float cCoeff = glm::dot(start, start) - radius * radius;
        const float discriminant = bCoeff * bCoeff - 4.0f * aCoeff * cCoeff;
        if (discriminant < 0.0f) {
            return std::nullopt;
        }
        const float time = (-bCoeff - std::sqrt(discriminant)) / (2.0f * aCoeff);
        if (time < 0.0f || time > 1.0f) {
            return std::nullopt;
        }
        Contact swept = base;
        swept.continuous = true;
        swept.timeOfImpact = time;
        swept.penetration = std::max(swept.penetration, 0.001f);
        return swept;
    }

    if (glm::dot(relativeStart, base.normal) > 0.0f && relativeSpeed > 0.0f) {
        const float closingSpeed = -glm::dot(relativeVelocity, base.normal);
        if (closingSpeed > kEpsilon) {
            const float time = glm::clamp(glm::dot(relativeStart, base.normal) / closingSpeed, 0.0f, 1.0f);
            Contact swept = base;
            swept.continuous = true;
            swept.timeOfImpact = time;
            swept.penetration = std::max(swept.penetration, 0.001f);
            return swept;
        }
    }
    (void)b;
    return std::nullopt;
}

float PhysicsSystem::inverseMass(uint32_t bodyIndex) const {
    const RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    return body.dynamic() ? body.inverseMass : 0.0f;
}

float PhysicsSystem::solverInverseMass(uint32_t bodyIndex) const {
    if (m_bodies[bodyIndex].characterDriven) {
        // Character-driven bodies are kinematic to the solver:
        // velocity and position corrections land entirely on the
        // OTHER body (a pushed cube still gets pushed), and the
        // character can never be displaced — let alone launched —
        // by contact resolution.
        return 0.0f;
    }
    const RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    if (body.sleeping) {
        return 0.0f; // asleep: immovable until the wake pass says otherwise
    }
    return body.dynamic() ? body.inverseMass : 0.0f;
}

glm::vec3 PhysicsSystem::angularVelocityRad(uint32_t bodyIndex) const {
    const RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    if (!body.dynamic()) {
        return glm::vec3{0.0f};
    }
    // Angular momentum is the conserved quantity the solver modifies, so
    // derive omega from it; fall back to the component's angular velocity
    // before the first integration seeds momentum.
    if (glm::dot(body.angularMomentum, body.angularMomentum) > kEpsilon) {
        return inverseInertiaWorld(bodyIndex, body.angularMomentum);
    }
    return glm::radians(body.angularVelocity);
}

glm::vec3 PhysicsSystem::pointVelocity(uint32_t bodyIndex, const glm::vec3& point) const {
    const RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    const glm::vec3 radius = point - m_bodies[bodyIndex].transform->position;
    return body.velocity + glm::cross(angularVelocityRad(bodyIndex), radius);
}

void PhysicsSystem::applyImpulseAtPoint(uint32_t bodyIndex, const glm::vec3& point, const glm::vec3& impulse,
                                        float sign) {
    RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    const float invMass = solverInverseMass(bodyIndex);
    if (invMass <= 0.0f) {
        return;
    }
    body.velocity += sign * impulse * invMass;
    // Torque coupling: an impulse off-center changes angular momentum,
    // which the integrator turns back into angular velocity. This is
    // what lets manifold points right a tipping box (or start one
    // rolling) instead of every contact pushing through the center.
    const glm::vec3 radius = point - m_bodies[bodyIndex].transform->position;
    body.angularMomentum += sign * glm::cross(radius, impulse);
}

void PhysicsSystem::wakeBody(uint32_t bodyIndex) {
    RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    body.sleeping = false;
    body.sleepTimer = 0.0f;
}

glm::vec3 PhysicsSystem::inverseInertiaWorld(uint32_t bodyIndex, const glm::vec3& vector) const {
    const RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    if (!body.dynamic()) {
        return glm::vec3{0.0f};
    }
    const glm::mat3 rotation = rotationMatrix(body.orientation);
    const glm::mat3 worldInverse = rotation * body.inverseInertiaTensor * glm::transpose(rotation);
    return worldInverse * vector;
}

std::optional<uint32_t> PhysicsSystem::bodyIndexFor(entt::entity entity) const {
    for (size_t i = 0; i < m_bodies.size(); ++i) {
        if (m_bodies[i].entity == entity) {
            return static_cast<uint32_t>(i);
        }
    }
    return std::nullopt;
}

void PhysicsSystem::prepareWarmStarting(const PhysicsSettings& settings) {
    // Phase 1: match stored impulses to this sub-step's contacts (by
    // pre-centroid feature point) and stage the stored values on
    // them. Nothing is applied yet: velocity targets are computed
    // from the pre-delivery state, and the staged set is applied
    // together afterwards. The stored impulses of a
    // support chain (capsule -> cube -> floor) are only meaningful
    // as a set; delivering them one by one would let a solver
    // iteration measure the light middle body's transient swing as
    // approach velocity and cash it in as kinetic energy.
    struct StagedWarm {
        Contact* contact;
    };
    std::vector<StagedWarm> staged;
    staged.reserve(m_contacts.size());
    for (Contact& contact : m_contacts) {
        const uint64_t key = (static_cast<uint64_t>(contact.bodyA) << 32u) | contact.bodyB;
        const glm::vec3 localPoint = glm::transpose(m_geometry[contact.bodyA].axes) *
                                     (contact.featurePoint - m_geometry[contact.bodyA].center);
        for (const WarmContact& warm : m_warmContacts) {
            if (warm.pairKey == key && glm::dot(warm.normal, contact.normal) > 0.5f &&
                glm::dot(warm.localPointA - localPoint, warm.localPointA - localPoint) < 0.0025f) {
                contact.persistent = true;
                contact.normalImpulse = warm.normalImpulse;
                contact.tangentImpulse = warm.tangentImpulse;
                staged.push_back(StagedWarm{&contact});
                break;
            }
        }
    }

    // Phase 2: velocity targets, from the pre-delivery state.
    // (Persistence is already resolved by phase 1, which the
    // restitution gating below depends on.)
    for (Contact& contact : m_contacts) {
        const glm::vec3 relativeVelocity =
            pointVelocity(contact.bodyB, contact.point) - pointVelocity(contact.bodyA, contact.point);
        const float vn = glm::dot(relativeVelocity, contact.normal);
        // Restitution fires only on a new impact faster than the rest
        // threshold. A persistent contact keeps target 0 no matter how
        // the island's other contacts shuffle velocities around: in a
        // stack, re-applying restitution against an already-resting
        // neighbour (backed by an immovable floor) is a launch pump.
        // Character-driven contacts NEVER restitute: the capsule is
        // a kinematic pusher, and a bounce target against it would
        // pump energy into the dynamic partner every sub-step.
        const bool characterContact =
            m_bodies[contact.bodyA].characterDriven || m_bodies[contact.bodyB].characterDriven;
        contact.restitutionTarget = !characterContact && !contact.persistent &&
                                            vn < -settings.restitutionVelocityThreshold
                                        ? -contact.restitution * vn
                                        : 0.0f;
    }

    // Phase 3: apply the staged set as one system. Full stored
    // impulses are delivered: with shock propagation (see
    // solveConstraints) every contact's iteration response matches
    // its actual constraint environment — a light body pinned by a
    // support chain presents only the heavy partner's inverse mass
    // — so if a delivered impulse overshoots the current approach
    // (e.g. one accumulated while a chain absorbed an impact, now
    // re-fired after the approach is gone), the first solver sweep
    // measures the true overshoot and the accumulated-impulse clamp
    // removes it within the same sub-step, before any of it can
    // integrate into a launch. Capping the delivery itself (by pair
    // response, by measured conductance, or by per-body speed) was
    // tried and rejected: each variant either starved legitimate
    // chain support or corrupted the impulse bookkeeping that
    // momentum conservation across sub-steps depends on.
    for (const StagedWarm& entry : staged) {
        applyWarmStart(*entry.contact);
    }
}

void PhysicsSystem::storeWarmStarting() {
    m_warmContacts.clear();
    m_warmContacts.reserve(m_contacts.size());
    for (const Contact& contact : m_contacts) {
        WarmContact warm;
        warm.pairKey = (static_cast<uint64_t>(contact.bodyA) << 32u) | contact.bodyB;
        warm.normal = contact.normal;
        // Manifold points share one centroid constraint, so each point's
        // accumulated impulse is one share of the pair total; every
        // point re-applies its own share next step (see prepare).
        warm.localPointA = glm::transpose(m_geometry[contact.bodyA].axes) *
                           (contact.featurePoint - m_geometry[contact.bodyA].center);
        warm.normalImpulse = contact.normalImpulse;
        warm.restitutionTarget = contact.restitutionTarget;
        warm.tangentImpulse = contact.tangentImpulse;
        m_warmContacts.push_back(warm);
    }
}

void PhysicsSystem::applyWarmStart(Contact& contact) {
    if (solverInverseMass(contact.bodyA) + solverInverseMass(contact.bodyB) <= 0.0f) {
        return;
    }
    // Deliver under the same mass fiction that produced the stored
    // impulse: a body pinned as a support in the velocity solve
    // (extreme-ratio chain) never received the impulse while it
    // accumulated, so it must not receive it now either — the stored
    // impulse is sized for the heavy partner's mass, and handing it
    // to the light support would fire it at J/m_light (thousands of
    // m/s) faster than the chain's iterations can absorb it.
    const glm::vec3 impulse = contact.normal * contact.normalImpulse + contact.tangentImpulse;
    if (contact.velInvMassA > 0.0f) {
        applyImpulseAtPoint(contact.bodyA, contact.point, impulse, -1.0f);
    }
    if (contact.velInvMassB > 0.0f) {
        applyImpulseAtPoint(contact.bodyB, contact.point, impulse, 1.0f);
    }
}

void PhysicsSystem::applyContinuousCollisionStops() {
    for (const Contact& contact : m_contacts) {
        if (!contact.continuous || contact.timeOfImpact >= 1.0f) {
            continue;
        }
        const float invA = solverInverseMass(contact.bodyA);
        const float invB = solverInverseMass(contact.bodyB);
        if (invA + invB <= 0.0f) {
            continue;
        }
        RigidBodyComponent& a = *m_bodies[contact.bodyA].body;
        RigidBodyComponent& b = *m_bodies[contact.bodyB].body;
        const float rollback = 1.0f - contact.timeOfImpact;
        if (invA > 0.0f) {
            m_bodies[contact.bodyA].transform->position -= m_geometry[contact.bodyA].displacement * rollback;
        }
        if (invB > 0.0f) {
            m_bodies[contact.bodyB].transform->position -= m_geometry[contact.bodyB].displacement * rollback;
        }
        const glm::vec3 relativeVelocity = b.velocity - a.velocity;
        const float vn = glm::dot(relativeVelocity, contact.normal);
        if (vn < 0.0f) {
            const float impulse = -vn / (invA + invB);
            a.velocity -= contact.normal * impulse * invA;
            b.velocity += contact.normal * impulse * invB;
        }
    }
}

void PhysicsSystem::buildIslands() {
    std::vector<std::vector<uint32_t>> contactBodies(m_contacts.size());
    for (size_t i = 0; i < m_contacts.size(); ++i) {
        contactBodies[i] = {m_contacts[i].bodyA, m_contacts[i].bodyB};
    }
    std::vector<std::vector<uint32_t>> bodyContacts(m_bodies.size());
    for (size_t i = 0; i < contactBodies.size(); ++i) {
        for (const uint32_t body : contactBodies[i]) {
            bodyContacts[body].push_back(static_cast<uint32_t>(i));
        }
    }
    for (size_t i = 0; i < m_joints.size(); ++i) {
        bodyContacts[m_joints[i].bodyA].push_back(static_cast<uint32_t>(m_contacts.size() + i));
        if (m_joints[i].hasBodyB) {
            bodyContacts[m_joints[i].bodyB].push_back(static_cast<uint32_t>(m_contacts.size() + i));
        }
    }

    std::vector<bool> visited(m_bodies.size(), false);
    std::vector<bool> contactSeen(m_contacts.size(), false);
    std::vector<bool> jointSeen(m_joints.size(), false);
    m_islands.clear();
    m_lastStats.largestIsland = 0;

    std::vector<uint32_t> stack;
    for (size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        if (visited[bodyIndex] || bodyContacts[bodyIndex].empty()) {
            continue;
        }
        SolverIsland island;
        size_t bodyCount = 0;
        stack.clear();
        stack.push_back(static_cast<uint32_t>(bodyIndex));
        visited[bodyIndex] = true;
        while (!stack.empty()) {
            const uint32_t current = stack.back();
            stack.pop_back();
            ++bodyCount;
            for (const uint32_t item : bodyContacts[current]) {
                if (item < m_contacts.size()) {
                    if (!contactSeen[item]) {
                        contactSeen[item] = true;
                        island.contacts.push_back(item);
                    }
                    for (const uint32_t neighbor : contactBodies[item]) {
                        if (!visited[neighbor]) {
                            visited[neighbor] = true;
                            stack.push_back(neighbor);
                        }
                    }
                } else {
                    const size_t jointIndex = item - m_contacts.size();
                    if (!jointSeen[jointIndex]) {
                        jointSeen[jointIndex] = true;
                        island.joints.push_back(jointIndex);
                    }
                    const JointRef& joint = m_joints[jointIndex];
                    if (!visited[joint.bodyA]) {
                        visited[joint.bodyA] = true;
                        stack.push_back(joint.bodyA);
                    }
                    if (joint.hasBodyB && !visited[joint.bodyB]) {
                        visited[joint.bodyB] = true;
                        stack.push_back(joint.bodyB);
                    }
                }
            }
        }
        m_lastStats.largestIsland = std::max(m_lastStats.largestIsland, bodyCount);
        m_islands.push_back(std::move(island));
    }
    m_lastStats.islands = m_islands.size();
}

void PhysicsSystem::solveConstraints(const PhysicsSettings& settings, JobSystem* jobs, float dt) {
    // Wake pass: a sleeping body struck hard enough (or deeply
    // penetrated, or dragged by an awake joint partner) rejoins the
    // simulation; gentle resting contact leaves it asleep and immovable.
    if (settings.sleepEnabled) {
        for (const Contact& contact : m_contacts) {
            RigidBodyComponent& a = *m_bodies[contact.bodyA].body;
            RigidBodyComponent& b = *m_bodies[contact.bodyB].body;
            if (a.sleeping == b.sleeping) {
                continue;
            }
            const float approachSpeed = -glm::dot(b.velocity - a.velocity, contact.normal);
            // Wake on a genuine impact, or on severe *and* moving
            // penetration. Penetration alone is not enough: positional
            // correction in a neighbouring awake body can transiently
            // deepen a sleeping body's contact on paper, and waking it
            // for that just re-ignites the jitter the sleep state was
            // supposed to end.
            const bool hardImpact = approachSpeed > settings.wakeSpeedThreshold;
            const bool severeMovingPenetration = contact.penetration > std::max(8.0f * settings.contactSlop, 0.02f) &&
                                                 approachSpeed > 0.05f;
            if (hardImpact || severeMovingPenetration) {
                wakeBody(a.sleeping ? contact.bodyA : contact.bodyB);
            }
        }
        for (const JointRef& joint : m_joints) {
            if (!joint.hasBodyB) {
                continue;
            }
            RigidBodyComponent& a = *m_bodies[joint.bodyA].body;
            RigidBodyComponent& b = *m_bodies[joint.bodyB].body;
            if (a.sleeping && !b.sleeping) {
                wakeBody(joint.bodyA);
            } else if (b.sleeping && !a.sleeping) {
                wakeBody(joint.bodyB);
            }
        }
    }

    // Support depths (shock propagation): a body's depth is the
    // length of the shortest contact chain from an immovable body
    // (static or asleep: solver inverse mass zero) up to it. Within
    // a contact between different depths, the shallower body is the
    // support and is treated as immovable (Contact::solveInvMass*):
    // plain Gauss-Seidel transfers only ~invM_heavy/invM_light of a
    // heavy body's approach through a light pinned body per sweep,
    // so at player-vs-cube mass ratios the heavy body tunnels
    // through the light one before the iterations can converge,
    // while pinning the support lets the reaction propagate one
    // chain level per sweep. Equal-depth contacts (free collisions,
    // side-by-side pushes) keep full two-body masses, so momentum
    // behaviour there is untouched.
    {
        const uint32_t kUnreached = std::numeric_limits<uint32_t>::max();
        std::vector<uint32_t> supportDepth(m_bodies.size(), kUnreached);
        for (size_t i = 0; i < m_bodies.size(); ++i) {
            if (solverInverseMass(static_cast<uint32_t>(i)) <= 0.0f) {
                supportDepth[i] = 0;
            }
        }
        for (size_t pass = 0; pass < m_bodies.size(); ++pass) {
            bool changed = false;
            for (const Contact& contact : m_contacts) {
                uint32_t& depthA = supportDepth[contact.bodyA];
                uint32_t& depthB = supportDepth[contact.bodyB];
                if (depthA != kUnreached && depthB > depthA + 1) {
                    depthB = depthA + 1;
                    changed = true;
                }
                if (depthB != kUnreached && depthA > depthB + 1) {
                    depthA = depthB + 1;
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
        for (Contact& contact : m_contacts) {
            contact.solveInvMassA = solverInverseMass(contact.bodyA);
            contact.solveInvMassB = solverInverseMass(contact.bodyB);
            contact.velInvMassA = contact.solveInvMassA;
            contact.velInvMassB = contact.solveInvMassB;
            // Pin the support (shallower) body: positionally at any
            // ratio, and for the velocity solve only when the deeper
            // body is at least 16x heavier (inverse-mass ratio), the
            // regime where Gauss-Seidel cannot converge in budget.
            if (supportDepth[contact.bodyA] < supportDepth[contact.bodyB]) {
                contact.solveInvMassA = 0.0f;
                if (contact.velInvMassB > 0.0f && contact.velInvMassA >= 16.0f * contact.velInvMassB) {
                    contact.velInvMassA = 0.0f;
                }
            } else if (supportDepth[contact.bodyB] < supportDepth[contact.bodyA]) {
                contact.solveInvMassB = 0.0f;
                if (contact.velInvMassA > 0.0f && contact.velInvMassB >= 16.0f * contact.velInvMassA) {
                    contact.velInvMassB = 0.0f;
                }
            }
        }
    }

    prepareWarmStarting(settings);
    prepareJointWarmStarting();
    const auto solveOneIsland = [this, &settings, dt](SolverIsland& island) {
        for (uint32_t iteration = 0; iteration < settings.solverIterations; ++iteration) {
            for (const size_t jointIndex : island.joints) {
                resolveJointVelocity(m_joints[jointIndex], dt);
            }
            for (const size_t contactIndex : island.contacts) {
                resolveVelocity(m_contacts[contactIndex], settings);
            }
        }
        for (const size_t jointIndex : island.joints) {
            resolveJointPosition(m_joints[jointIndex]);
        }
        // Non-linear position correction: iterate the projection so the
        // remaining penetration is re-measured after every pass.
        const uint32_t positionIterations = std::max(1u, settings.positionIterations);
        for (uint32_t iteration = 0; iteration < positionIterations; ++iteration) {
            for (const size_t contactIndex : island.contacts) {
                resolvePosition(m_contacts[contactIndex], settings);
            }
        }
    };

    if (jobs != nullptr && m_islands.size() > 1) {
        const size_t islandCount = m_islands.size();
        jobs->parallelFor(islandCount, 0, [this, &solveOneIsland](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                solveOneIsland(m_islands[i]);
            }
        });
    } else {
        for (SolverIsland& island : m_islands) {
            solveOneIsland(island);
        }
    }
    applyRestingContactDamping(settings, dt);
    storeWarmStarting();
}

void PhysicsSystem::applyRestingContactDamping(const PhysicsSettings& settings, float dt) {
    if (dt <= 0.0f || (settings.contactLinearDamping <= 0.0f && settings.contactAngularDamping <= 0.0f)) {
        return;
    }
    std::vector<char> inContact(m_bodies.size(), 0);
    for (const Contact& contact : m_contacts) {
        // Only persistent, weight-bearing contacts qualify: a contact
        // born this sub-step is an impact whose post-solve velocity is
        // the solver's exact answer (bleeding it would break momentum
        // conservation), and a side contact (wall, zero-gravity bump)
        // is not "resting" on anything. Stacks and floor contacts have
        // near-vertical normals and persist across sub-steps.
        if (!contact.persistent || std::abs(contact.normal.y) < 0.5f) {
            continue;
        }
        inContact[contact.bodyA] = 1;
        inContact[contact.bodyB] = 1;
    }
    const float linearFactor = std::exp(-std::max(settings.contactLinearDamping, 0.0f) * dt);
    const float angularFactor = std::exp(-std::max(settings.contactAngularDamping, 0.0f) * dt);
    for (size_t i = 0; i < m_bodies.size(); ++i) {
        if (inContact[i] == 0) {
            continue;
        }
        RigidBodyComponent& body = *m_bodies[i].body;
        if (!body.dynamic() || body.sleeping) {
            continue;
        }
        // A body under an applied force/torque is being driven, not
        // jittering: never bleed driven motion.
        if (glm::dot(body.force, body.force) > kEpsilon || glm::dot(body.torque, body.torque) > kEpsilon) {
            continue;
        }
        // Residual motion only: genuine impacts and fast slides are
        // the solver's business, not the damper's. Sleep-eligible
        // bodies are damped across the whole residual band; a
        // non-sleeping body (the player, allowSleep = false) only
        // inside the hard rest band below the static-stick threshold,
        // where nothing but solver residue lives — a controller
        // driving it by velocity override commands cruise speeds far
        // above the band, and force-driven bodies are vetoed above.
        // Without this, a capsule standing on a sleeping stack keeps
        // a sub-mm/s micro-roll (its spherical bottom rolls without
        // slipping, so point friction cannot oppose it) and creeps
        // sideways forever; inside the band the creep is damped to
        // exact zero within a few sub-steps.
        const float speedLimit = body.allowSleep ? 0.3f : 0.05f;
        if (glm::length(body.velocity) > speedLimit || glm::length(body.angularVelocity) > 90.0f) {
            continue;
        }
        body.velocity *= linearFactor;
        body.angularVelocity *= angularFactor;
        body.angularMomentum *= angularFactor;
        // Snap the last whisper of motion to exact zero so sleep
        // bookkeeping (and the renderer) sees a truly still body.
        if (glm::length(body.velocity) < 0.01f && glm::length(body.angularVelocity) < 1.0f) {
            body.velocity = glm::vec3{0.0f};
            body.angularVelocity = glm::vec3{0.0f};
            body.angularMomentum = glm::vec3{0.0f};
        }
    }
}

void PhysicsSystem::resolveVelocity(Contact& contact, const PhysicsSettings& settings) {
    // Velocity-solve masses: at extreme mass ratios within a support
    // chain the support is pinned (see solveConstraints), so the
    // impulse is sized by — and delivered only to — the heavy body;
    // comparable-mass contacts keep symmetric two-body masses.
    const float invA = contact.velInvMassA;
    const float invB = contact.velInvMassB;
    if (invA + invB <= 0.0f) {
        return; // static pair, or both asleep: nothing can move
    }
    (void)settings;

    const glm::vec3 normal = contact.normal;
    const glm::vec3 radiusA = contact.point - m_bodies[contact.bodyA].transform->position;
    const glm::vec3 radiusB = contact.point - m_bodies[contact.bodyB].transform->position;

    // Effective mass along a direction at the contact point:
    // 1 / (invM + n . ((I^-1 (r x n)) x r)) per body. This is the exact
    // mass-ratio weighting between translation and rotation, so a light
    // body striking a heavy body's edge responds correctly in both.
    const auto effectiveMass = [this](uint32_t bodyIndex, const glm::vec3& radius, const glm::vec3& direction,
                                      float invMass) {
        if (invMass <= 0.0f) {
            return 0.0f;
        }
        const glm::vec3 rCrossDir = glm::cross(radius, direction);
        return invMass + glm::dot(direction, glm::cross(inverseInertiaWorld(bodyIndex, rCrossDir), radius));
    };
    const auto applyDelta = [this, &contact](const glm::vec3& impulse) {
        // A body pinned as this contact's support (extreme-ratio
        // chain) receives none of the impulse: the reaction is
        // absorbed by the support chain beneath it. Applying it
        // would hand the light support the heavy body's momentum as
        // a transient velocity spike its own support contact then
        // has to absorb within the same sweep budget.
        if (contact.velInvMassA > 0.0f) {
            applyImpulseAtPoint(contact.bodyA, contact.point, impulse, -1.0f);
        }
        if (contact.velInvMassB > 0.0f) {
            applyImpulseAtPoint(contact.bodyB, contact.point, impulse, 1.0f);
        }
    };

    const float normalMass = effectiveMass(contact.bodyA, radiusA, normal, invA) +
                             effectiveMass(contact.bodyB, radiusB, normal, invB);
    if (normalMass <= 0.0f) {
        return;
    }
    {
        // Accumulated sequential impulse at the contact point: the total
        // is clamped to the compressive half-space, each iteration
        // applies only the delta (warm-start friendly, no stack bounce).
        const glm::vec3 relativeVelocity =
            pointVelocity(contact.bodyB, contact.point) - pointVelocity(contact.bodyA, contact.point);
        const float velocityAlongNormal = glm::dot(relativeVelocity, normal);
        const float lambda = (contact.restitutionTarget - velocityAlongNormal) / normalMass;
        const float newImpulse = std::max(0.0f, contact.normalImpulse + lambda);
        const float deltaImpulse = newImpulse - contact.normalImpulse;
        contact.normalImpulse = newImpulse;
        if (deltaImpulse != 0.0f) {
            applyDelta(normal * deltaImpulse);
        }
    }
    if (contact.friction <= 0.0f) {
        return;
    }
    // Anisotropic Coulomb friction at the contact point: two tangent
    // axes, each with its own friction coefficient and its own
    // point-effective mass, accumulated and clamped per axis to
    // mu * normalImpulse (the friction cone, axis-decomposed). For the
    // player capsule this produces smooth wall/floor sliding: tangential
    // motion is damped per axis without ever sticking the normal solve.
    const glm::vec3 relativeVelocity =
        pointVelocity(contact.bodyB, contact.point) - pointVelocity(contact.bodyA, contact.point);
    const glm::vec3 tangentialVelocity = relativeVelocity - normal * glm::dot(relativeVelocity, normal);
    const float tangentialSpeed = glm::length(tangentialVelocity);
    if (tangentialSpeed <= kEpsilon && glm::dot(contact.tangentImpulse, contact.tangentImpulse) <= kEpsilon) {
        return;
    }
    const glm::vec3 tangent = tangentialSpeed > kEpsilon
                                  ? tangentialVelocity / tangentialSpeed
                                  : glm::normalize(glm::cross(normal, glm::vec3{0.0f, 1.0f, 0.0f}) +
                                                   glm::vec3{kEpsilon});
    const glm::vec3 tangent2 = glm::normalize(glm::cross(normal, tangent));
    const float friction1 = contact.anisotropicFriction.x >= 0.0f ? contact.anisotropicFriction.x : contact.friction;
    const float friction2 = contact.anisotropicFriction.y >= 0.0f ? contact.anisotropicFriction.y : contact.friction;
    const float tangentMass1 = effectiveMass(contact.bodyA, radiusA, tangent, invA) +
                               effectiveMass(contact.bodyB, radiusB, tangent, invB);
    const float tangentMass2 = effectiveMass(contact.bodyA, radiusA, tangent2, invA) +
                               effectiveMass(contact.bodyB, radiusB, tangent2, invB);
    const float maxFriction1 = std::max(contact.normalImpulse, 0.0f) * friction1;
    const float maxFriction2 = std::max(contact.normalImpulse, 0.0f) * friction2;
    const float old1 = glm::dot(contact.tangentImpulse, tangent);
    const float old2 = glm::dot(contact.tangentImpulse, tangent2);
    const float candidate1 =
        tangentMass1 > 0.0f ? old1 - glm::dot(relativeVelocity, tangent) / tangentMass1 : old1;
    const float candidate2 =
        tangentMass2 > 0.0f ? old2 - glm::dot(relativeVelocity, tangent2) / tangentMass2 : old2;
    const glm::vec3 clamped = tangent * std::clamp(candidate1, -maxFriction1, maxFriction1) +
                              tangent2 * std::clamp(candidate2, -maxFriction2, maxFriction2);
    const glm::vec3 deltaImpulse = clamped - contact.tangentImpulse;
    contact.tangentImpulse = clamped;
    if (glm::dot(deltaImpulse, deltaImpulse) > 0.0f) {
        applyDelta(deltaImpulse);
    }
}

void PhysicsSystem::resolvePosition(Contact& contact, const PhysicsSettings& settings) {
    // Non-linear positional projection: each call corrects only a
    // clamped share of the *remaining* penetration, then re-measures
    // (bookkeeps) what is left. solveConstraints iterates this, so a
    // deep landing penetration converges geometrically over
    // positionIterations passes instead of receiving one violent
    // full-depth shove, and a resting contact stops exactly at the slop.
    if (contact.penetration <= settings.contactSlop) {
        return;
    }
    // Shock-propagation masses (see solveConstraints): the pinned
    // support of a chain does not move, so the deeper body takes the
    // whole correction — this is what lets a heavy body sunk into a
    // light supported one climb back out instead of the correction
    // being vetoed by the support contact beneath the light body.
    const float invA = contact.solveInvMassA;
    const float invB = contact.solveInvMassB;
    const float inverseMassSum = invA + invB;
    if (inverseMassSum <= 0.0f) {
        return;
    }
    float separationGain = (contact.penetration - settings.contactSlop) *
                           std::clamp(settings.positionCorrection, 0.0f, 1.0f);
    separationGain = std::min(separationGain, std::max(settings.maxPositionCorrection, 0.0f));
    separationGain /= static_cast<float>(std::max<uint8_t>(contact.manifoldCount, 1));
    if (separationGain <= 0.0f) {
        return;
    }
    // Positional impulse at the contact point: the correction is
    // distributed between translation and rotation through the same
    // point Jacobian the velocity solver uses (response = invM +
    // n . ((I^-1 (r x n)) x r) per body). Pure COM translation — the
    // previous form — is an energy pump for rotating bodies: a
    // tumbling cube's corner digs in a little more every sub-step,
    // and lifting the centre of mass straight out of the penetration
    // converts solver work into gravitational potential that gravity
    // immediately converts back into spin, sustaining (even
    // amplifying) the tumble indefinitely. With the rotational share
    // the correction mostly un-rotates the dig-in that created the
    // penetration, which adds no potential energy; face contacts
    // (r x n ~ 0 at the centroid) keep the old pure-translation
    // behaviour, so resting stacks are unaffected.
    const glm::vec3 radiusA = contact.point - m_bodies[contact.bodyA].transform->position;
    const glm::vec3 radiusB = contact.point - m_bodies[contact.bodyB].transform->position;
    const auto positionalResponse = [this](uint32_t bodyIndex, float invMass, const glm::vec3& radius,
                                           const glm::vec3& direction) {
        if (invMass <= 0.0f) {
            return 0.0f;
        }
        const glm::vec3 rCrossDir = glm::cross(radius, direction);
        return invMass + glm::dot(direction, glm::cross(inverseInertiaWorld(bodyIndex, rCrossDir), radius));
    };
    const float responseSum = positionalResponse(contact.bodyA, invA, radiusA, contact.normal) +
                              positionalResponse(contact.bodyB, invB, radiusB, contact.normal);
    if (responseSum <= kEpsilon) {
        return;
    }
    // Applies a positional impulse at the contact point and returns the
    // displacement it produced at that point (translation plus the
    // rotational share), so callers can measure the correction's
    // tangential side effects.
    const auto applyPositional = [this](uint32_t bodyIndex, float invMass, const glm::vec3& radius,
                                        const glm::vec3& impulse) {
        if (invMass <= 0.0f) {
            return glm::vec3{0.0f};
        }
        BodyRef& ref = m_bodies[bodyIndex];
        const glm::vec3 translation = impulse * invMass;
        ref.transform->position += translation;
        const glm::vec3 angularStep = inverseInertiaWorld(bodyIndex, glm::cross(radius, impulse));
        const float angle = glm::length(angularStep);
        if (angle > kEpsilon) {
            const glm::quat delta = glm::angleAxis(angle, angularStep / angle);
            ref.body->orientation = glm::normalize(delta * ref.body->orientation);
            ref.transform->setOrientation(ref.body->orientation);
        }
        return translation + glm::cross(angularStep, radius);
    };
    const float positionalImpulse = separationGain / responseSum;
    const glm::vec3 translationA = contact.normal * (-positionalImpulse * invA);
    const glm::vec3 translationB = contact.normal * (positionalImpulse * invB);
    applyPositional(contact.bodyA, invA, radiusA, contact.normal * (-positionalImpulse));
    applyPositional(contact.bodyB, invB, radiusB, contact.normal * positionalImpulse);
    contact.penetration -= separationGain; // re-measured remainder for the next iteration

    // Static-friction stick (positional tangential projection): for a
    // persistent, support-like contact in the static regime — relative
    // tangential point velocity below the zero-velocity threshold —
    // cancel the gravity-lateral part of the relative translation the
    // separation just produced, so the correction lifts the bodies
    // straight out of the support instead of walking them sideways.
    // Without this, a body resting across a tilted contact (a capsule
    // bottom wedged on a box edge or straddling the seam between two
    // cubes) ratchets laterally forever: gravity re-seats it
    // vertically every sub-step, the positional pass lifts it back
    // out along the tilted normal, and the asymmetry between the two
    // paths is a slow, endless creep. The contact point itself never
    // slides (measured slide ~ 0) — the body effectively rolls — so
    // velocity friction cannot oppose it; it acts on velocities,
    // after which the positions still move. Cancelling exactly the
    // lateral translation this correction introduced is what static
    // friction does physically: the resting body keeps its lateral
    // position while still separating vertically. Genuinely sliding
    // contacts (tangential speed above threshold), fresh impacts
    // (not persistent) and wall-like contacts (normal across
    // gravity) are untouched, and the stick never pulls a body
    // toward an anchor — it only removes the sideways motion the
    // separation itself introduced.
    if (contact.persistent && glm::dot(settings.gravity, settings.gravity) > kEpsilon) {
        const glm::vec3 gravityDir = glm::normalize(settings.gravity);
        if (std::abs(glm::dot(contact.normal, gravityDir)) > 0.5f) {
            const glm::vec3 relativeVelocity =
                pointVelocity(contact.bodyB, contact.point) - pointVelocity(contact.bodyA, contact.point);
            const glm::vec3 tangentialVelocity =
                relativeVelocity - contact.normal * glm::dot(relativeVelocity, contact.normal);
            constexpr float kStaticStickVelocityThreshold = 0.05f; // m/s
            if (glm::length(tangentialVelocity) < kStaticStickVelocityThreshold) {
                const glm::vec3 relativeTranslation = translationB - translationA;
                const glm::vec3 lateral =
                    relativeTranslation - gravityDir * glm::dot(relativeTranslation, gravityDir);
                if (glm::length(lateral) > 1e-7f) {
                    m_bodies[contact.bodyA].transform->position += lateral * (invA / inverseMassSum);
                    m_bodies[contact.bodyB].transform->position -= lateral * (invB / inverseMassSum);
                }
            }
        }
    }
}

void PhysicsSystem::prepareJointWarmStarting() {
    for (const JointRef& ref : m_joints) {
        JointComponent& joint = *ref.joint;
        if (joint.type == JointType::SpringDamper) {
            continue;
        }
        const auto found = m_jointStates.find(entt::to_integral(ref.jointEntity));
        if (found == m_jointStates.end()) {
            continue;
        }
        const JointSolverState& state = found->second;
        RigidBodyComponent& bodyA = *m_bodies[ref.bodyA].body;
        const glm::vec3 pointA = m_bodies[ref.bodyA].transform->position + bodyA.orientation * joint.anchorA;
        glm::vec3 pointB = joint.anchorB;
        glm::quat orientationB{1.0f, 0.0f, 0.0f, 0.0f};
        if (ref.hasBodyB) {
            RigidBodyComponent& bodyB = *m_bodies[ref.bodyB].body;
            pointB = m_bodies[ref.bodyB].transform->position + bodyB.orientation * joint.anchorB;
            orientationB = bodyB.orientation;
        }
        // Re-apply the accumulated impulses through the CURRENT
        // Jacobians, as one set, before any velocity error is
        // measured — the same protocol as contact warm starting.
        if (joint.type == JointType::Distance) {
            const glm::vec3 axis = pointB - pointA;
            const float distance = glm::length(axis);
            if (distance > kEpsilon && state.distanceImpulse != 0.0f) {
                const glm::vec3 impulse = (axis / distance) * state.distanceImpulse;
                applyImpulseAtPoint(ref.bodyA, pointA, impulse, -1.0f);
                if (ref.hasBodyB) {
                    applyImpulseAtPoint(ref.bodyB, pointB, impulse, 1.0f);
                }
            }
            continue;
        }
        if (glm::dot(state.pointImpulse, state.pointImpulse) > 0.0f) {
            applyImpulseAtPoint(ref.bodyA, pointA, state.pointImpulse, -1.0f);
            if (ref.hasBodyB) {
                applyImpulseAtPoint(ref.bodyB, pointB, state.pointImpulse, 1.0f);
            }
        }
        glm::vec3 angularImpulse = state.angularImpulse;
        if (joint.type == JointType::Hinge) {
            const glm::vec3 axisLocal = glm::dot(joint.hingeAxis, joint.hingeAxis) > kEpsilon
                                            ? glm::normalize(joint.hingeAxis)
                                            : glm::vec3{0.0f, 0.0f, 1.0f};
            const glm::vec3 axisA = glm::normalize(bodyA.orientation * axisLocal);
            glm::vec3 tangent1;
            glm::vec3 tangent2;
            perpendicularBasis(axisA, tangent1, tangent2);
            angularImpulse = tangent1 * state.axisImpulse.x + tangent2 * state.axisImpulse.y +
                             axisA * state.limitImpulse;
        }
        if (glm::dot(angularImpulse, angularImpulse) > 0.0f) {
            if (solverInverseMass(ref.bodyA) > 0.0f) {
                bodyA.angularMomentum -= angularImpulse;
            }
            if (ref.hasBodyB && solverInverseMass(ref.bodyB) > 0.0f) {
                m_bodies[ref.bodyB].body->angularMomentum += angularImpulse;
            }
        }
    }
}

void PhysicsSystem::solveJointLinearRow(const JointRef& ref, const glm::vec3& pointA, const glm::vec3& pointB,
                                        const glm::vec3& direction, float biasVelocity, float& accumulated,
                                        float minAccumulated, float maxAccumulated) {
    // Cap the Baumgarte correction speed: an unclamped beta*C/dt bias
    // turns a large positional error (teleport, spawn overlap) into an
    // equally large target velocity and launches the bodies; the
    // position pass removes bulk error without adding energy.
    biasVelocity = glm::clamp(biasVelocity, -2.0f, 2.0f);
    const float invA = solverInverseMass(ref.bodyA);
    const float invB = ref.hasBodyB ? solverInverseMass(ref.bodyB) : 0.0f;
    // Effective-mass denominator: inverse masses plus the angular
    // response of both bodies to an impulse at their anchor points.
    float denominator = invA + invB;
    if (invA > 0.0f) {
        const glm::vec3 lever = glm::cross(pointA - m_bodies[ref.bodyA].transform->position, direction);
        denominator += glm::dot(lever, inverseInertiaWorld(ref.bodyA, lever));
    }
    if (ref.hasBodyB && invB > 0.0f) {
        const glm::vec3 lever = glm::cross(pointB - m_bodies[ref.bodyB].transform->position, direction);
        denominator += glm::dot(lever, inverseInertiaWorld(ref.bodyB, lever));
    }
    if (denominator <= kEpsilon) {
        return;
    }
    glm::vec3 relativeVelocity = -pointVelocity(ref.bodyA, pointA);
    if (ref.hasBodyB) {
        relativeVelocity += pointVelocity(ref.bodyB, pointB);
    }
    const float lambda = -(glm::dot(relativeVelocity, direction) + biasVelocity) / denominator;
    const float newAccumulated = glm::clamp(accumulated + lambda, minAccumulated, maxAccumulated);
    const float applied = newAccumulated - accumulated;
    accumulated = newAccumulated;
    if (applied == 0.0f) {
        return;
    }
    const glm::vec3 impulse = direction * applied;
    if (invA > 0.0f) {
        applyImpulseAtPoint(ref.bodyA, pointA, impulse, -1.0f);
    }
    if (ref.hasBodyB && invB > 0.0f) {
        applyImpulseAtPoint(ref.bodyB, pointB, impulse, 1.0f);
    }
}

void PhysicsSystem::solveJointAngularRow(const JointRef& ref, const glm::vec3& axis, float biasVelocity,
                                         float& accumulated, float minAccumulated, float maxAccumulated) {
    biasVelocity = glm::clamp(biasVelocity, -2.0f, 2.0f); // see solveJointLinearRow
    const bool activeA = solverInverseMass(ref.bodyA) > 0.0f;
    const bool activeB = ref.hasBodyB && solverInverseMass(ref.bodyB) > 0.0f;
    float denominator = 0.0f;
    if (activeA) {
        denominator += glm::dot(axis, inverseInertiaWorld(ref.bodyA, axis));
    }
    if (activeB) {
        denominator += glm::dot(axis, inverseInertiaWorld(ref.bodyB, axis));
    }
    if (denominator <= kEpsilon) {
        return;
    }
    glm::vec3 relativeAngular = -angularVelocityRad(ref.bodyA);
    if (ref.hasBodyB) {
        relativeAngular += angularVelocityRad(ref.bodyB);
    }
    const float lambda = -(glm::dot(relativeAngular, axis) + biasVelocity) / denominator;
    const float newAccumulated = glm::clamp(accumulated + lambda, minAccumulated, maxAccumulated);
    const float applied = newAccumulated - accumulated;
    accumulated = newAccumulated;
    if (applied == 0.0f) {
        return;
    }
    const glm::vec3 impulse = axis * applied;
    if (activeA) {
        m_bodies[ref.bodyA].body->angularMomentum -= impulse;
    }
    if (activeB) {
        m_bodies[ref.bodyB].body->angularMomentum += impulse;
    }
}

void PhysicsSystem::resolveJointVelocity(const JointRef& ref, float dt) {
    JointComponent& joint = *ref.joint;
    if (joint.type == JointType::SpringDamper) {
        return; // spring-dampers are force constraints, applied before integration
    }
    if (solverInverseMass(ref.bodyA) <= 0.0f && (!ref.hasBodyB || solverInverseMass(ref.bodyB) <= 0.0f)) {
        return; // both sides immovable (static or asleep): nothing can move
    }
    JointSolverState& state = m_jointStates[entt::to_integral(ref.jointEntity)];
    RigidBodyComponent& bodyA = *m_bodies[ref.bodyA].body;
    const glm::quat orientationA = bodyA.orientation;
    const glm::vec3 pointA = m_bodies[ref.bodyA].transform->position + orientationA * joint.anchorA;
    glm::vec3 pointB = joint.anchorB;
    glm::quat orientationB{1.0f, 0.0f, 0.0f, 0.0f};
    if (ref.hasBodyB) {
        RigidBodyComponent& bodyB = *m_bodies[ref.bodyB].body;
        pointB = m_bodies[ref.bodyB].transform->position + bodyB.orientation * joint.anchorB;
        orientationB = bodyB.orientation;
    }
    // Baumgarte-style error feedback, softened by compliance.
    const float compliance = glm::clamp(joint.compliance, 0.0f, 1.0f);
    const float biasScale = dt > 0.0f ? 0.2f * (1.0f - 0.9f * compliance) / dt : 0.0f;
    const float kInfinite = std::numeric_limits<float>::infinity();

    if (joint.type == JointType::Distance) {
        const glm::vec3 axis = pointB - pointA;
        const float distance = glm::length(axis);
        if (distance <= kEpsilon) {
            return;
        }
        const glm::vec3 direction = axis / distance;
        const bool bounded = joint.minDistance >= 0.0f || joint.maxDistance >= 0.0f;
        if (!bounded) {
            solveJointLinearRow(ref, pointA, pointB, direction, biasScale * (distance - ref.restLength),
                                state.distanceImpulse, -kInfinite, kInfinite);
        } else if (joint.maxDistance >= 0.0f && distance > joint.maxDistance) {
            // Rope taut: only pull together (accumulated <= 0).
            solveJointLinearRow(ref, pointA, pointB, direction, biasScale * (distance - joint.maxDistance),
                                state.distanceImpulse, -kInfinite, 0.0f);
        } else if (joint.minDistance >= 0.0f && distance < joint.minDistance) {
            // Strut compressed: only push apart (accumulated >= 0).
            solveJointLinearRow(ref, pointA, pointB, direction, biasScale * (distance - joint.minDistance),
                                state.distanceImpulse, 0.0f, kInfinite);
        } else {
            state.distanceImpulse = 0.0f; // slack: forget the warm impulse
        }
        return;
    }

    // Hinge and Fixed share the anchor-coincidence (point) rows.
    const glm::vec3 anchorError = pointB - pointA;
    const glm::vec3 basis[3] = {glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f},
                                glm::vec3{0.0f, 0.0f, 1.0f}};
    for (int row = 0; row < 3; ++row) {
        solveJointLinearRow(ref, pointA, pointB, basis[row], biasScale * anchorError[row],
                            state.pointImpulse[row], -kInfinite, kInfinite);
    }

    if (joint.type == JointType::Hinge) {
        const glm::vec3 axisLocal = glm::dot(joint.hingeAxis, joint.hingeAxis) > kEpsilon
                                        ? glm::normalize(joint.hingeAxis)
                                        : glm::vec3{0.0f, 0.0f, 1.0f};
        const glm::vec3 axisA = glm::normalize(orientationA * axisLocal);
        const glm::vec3 axisB = glm::normalize(orientationB * axisLocal);
        glm::vec3 tangent1;
        glm::vec3 tangent2;
        perpendicularBasis(axisA, tangent1, tangent2);
        // Lock the two angular degrees of freedom perpendicular to
        // the hinge axis; rotation about the axis itself stays free.
        const glm::vec3 misalignment = glm::cross(axisA, axisB);
        solveJointAngularRow(ref, tangent1, biasScale * glm::dot(misalignment, tangent1), state.axisImpulse.x,
                             -kInfinite, kInfinite);
        solveJointAngularRow(ref, tangent2, biasScale * glm::dot(misalignment, tangent2), state.axisImpulse.y,
                             -kInfinite, kInfinite);
        if (joint.enableLimits && state.orientationCaptured) {
            const glm::quat relative = glm::normalize(glm::inverse(orientationA) * orientationB);
            const float angle = twistAngleDegrees(relative, state.capturedRelativeOrientation, axisLocal);
            if (angle > joint.maxAngleDegrees) {
                solveJointAngularRow(ref, axisA, biasScale * glm::radians(angle - joint.maxAngleDegrees),
                                     state.limitImpulse, -kInfinite, 0.0f);
            } else if (angle < joint.minAngleDegrees) {
                solveJointAngularRow(ref, axisA, biasScale * glm::radians(angle - joint.minAngleDegrees),
                                     state.limitImpulse, 0.0f, kInfinite);
            } else {
                state.limitImpulse = 0.0f;
            }
        }
        return;
    }

    // Fixed: lock the relative orientation to the captured one. The
    // error is measured on body B against its target orientation
    // (for a world joint, body B is the identity frame and the same
    // formula reduces to locking body A's captured orientation).
    if (state.orientationCaptured) {
        const glm::quat targetB = glm::normalize(orientationA * state.capturedRelativeOrientation);
        const glm::vec3 error = quatErrorVector(glm::normalize(orientationB * glm::inverse(targetB)));
        for (int row = 0; row < 3; ++row) {
            solveJointAngularRow(ref, basis[row], biasScale * error[row], state.angularImpulse[row],
                                 -kInfinite, kInfinite);
        }
    }
}

void PhysicsSystem::rotateJointBody(uint32_t bodyIndex, const glm::quat& worldDelta) {
    if (solverInverseMass(bodyIndex) <= 0.0f) {
        return;
    }
    RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    body.orientation = glm::normalize(worldDelta * body.orientation);
    m_bodies[bodyIndex].transform->setOrientation(body.orientation);
}

void PhysicsSystem::resolveJointPosition(const JointRef& ref) {
    JointComponent& joint = *ref.joint;
    if (joint.type == JointType::SpringDamper) {
        return;
    }
    const float invA = solverInverseMass(ref.bodyA);
    const float invB = ref.hasBodyB ? solverInverseMass(ref.bodyB) : 0.0f;
    const float inverseMassSum = invA + invB;
    if (inverseMassSum <= 0.0f) {
        return;
    }
    RigidBodyComponent& bodyA = *m_bodies[ref.bodyA].body;
    const glm::quat orientationA = bodyA.orientation;
    const glm::vec3 pointA = m_bodies[ref.bodyA].transform->position + orientationA * joint.anchorA;
    glm::vec3 pointB = joint.anchorB;
    glm::quat orientationB{1.0f, 0.0f, 0.0f, 0.0f};
    if (ref.hasBodyB) {
        RigidBodyComponent& bodyB = *m_bodies[ref.bodyB].body;
        pointB = m_bodies[ref.bodyB].transform->position + bodyB.orientation * joint.anchorB;
        orientationB = bodyB.orientation;
    }
    const float compliance = glm::clamp(joint.compliance, 0.0f, 1.0f);
    const float strength = 0.8f * (1.0f - 0.9f * compliance);

    if (joint.type == JointType::Distance) {
        const glm::vec3 axis = pointB - pointA;
        const float distance = glm::length(axis);
        if (distance <= kEpsilon) {
            return;
        }
        float error = distance - ref.restLength;
        if (joint.minDistance >= 0.0f || joint.maxDistance >= 0.0f) {
            error = 0.0f;
            if (joint.maxDistance >= 0.0f && distance > joint.maxDistance) {
                error = distance - joint.maxDistance;
            } else if (joint.minDistance >= 0.0f && distance < joint.minDistance) {
                error = distance - joint.minDistance;
            }
            if (error == 0.0f) {
                return;
            }
        }
        const glm::vec3 correction = (axis / distance) * (error * strength);
        m_bodies[ref.bodyA].transform->position += correction * (invA / inverseMassSum);
        if (ref.hasBodyB) {
            m_bodies[ref.bodyB].transform->position -= correction * (invB / inverseMassSum);
        }
        return;
    }

    // Hinge/Fixed: project the anchor points together (mass-shared
    // translation; orientation is handled below).
    const glm::vec3 delta = (pointB - pointA) * strength;
    m_bodies[ref.bodyA].transform->position += delta * (invA / inverseMassSum);
    if (ref.hasBodyB) {
        m_bodies[ref.bodyB].transform->position -= delta * (invB / inverseMassSum);
    }

    const auto inertiaWeight = [this](uint32_t bodyIndex, const glm::vec3& axis) {
        return solverInverseMass(bodyIndex) > 0.0f
                   ? std::max(0.0f, glm::dot(axis, inverseInertiaWorld(bodyIndex, axis)))
                   : 0.0f;
    };

    if (joint.type == JointType::Hinge) {
        const glm::vec3 axisLocal = glm::dot(joint.hingeAxis, joint.hingeAxis) > kEpsilon
                                        ? glm::normalize(joint.hingeAxis)
                                        : glm::vec3{0.0f, 0.0f, 1.0f};
        const glm::vec3 axisA = glm::normalize(orientationA * axisLocal);
        const glm::vec3 axisB = glm::normalize(orientationB * axisLocal);
        const glm::vec3 cross = glm::cross(axisB, axisA);
        const float sine = glm::length(cross);
        if (sine > 1e-6f) {
            const glm::vec3 axis = cross / sine;
            const float angle = std::atan2(sine, glm::clamp(glm::dot(axisA, axisB), -1.0f, 1.0f));
            const float weightA = inertiaWeight(ref.bodyA, axis);
            const float weightB = ref.hasBodyB ? inertiaWeight(ref.bodyB, axis) : 0.0f;
            const float weightSum = weightA + weightB;
            if (weightSum > kEpsilon) {
                // Rotating B about cross(B, A) by +angle aligns the
                // axes; A counter-rotates by its inertia share.
                if (ref.hasBodyB) {
                    rotateJointBody(ref.bodyB, glm::angleAxis(angle * strength * (weightB / weightSum), axis));
                }
                rotateJointBody(ref.bodyA, glm::angleAxis(-angle * strength * (weightA / weightSum), axis));
            }
        }
        return;
    }

    // Fixed: rotate both bodies toward the captured relative
    // orientation (the rotation taking B to its target, shared by
    // inverse inertia; a world joint spends it all on body A).
    const auto found = m_jointStates.find(entt::to_integral(ref.jointEntity));
    if (found == m_jointStates.end() || !found->second.orientationCaptured) {
        return;
    }
    const glm::quat captured = found->second.capturedRelativeOrientation;
    glm::quat deltaRotation;
    if (ref.hasBodyB) {
        const glm::quat targetB = glm::normalize(orientationA * captured);
        deltaRotation = glm::normalize(targetB * glm::inverse(orientationB));
    } else {
        const glm::quat targetA = glm::normalize(glm::inverse(captured));
        deltaRotation = glm::normalize(targetA * glm::inverse(orientationA));
    }
    glm::vec3 axis;
    float angle = 0.0f;
    quatToAxisAngle(deltaRotation, axis, angle);
    if (angle <= 1e-6f) {
        return;
    }
    if (!ref.hasBodyB) {
        rotateJointBody(ref.bodyA, glm::angleAxis(angle * strength, axis));
        return;
    }
    const float weightA = inertiaWeight(ref.bodyA, axis);
    const float weightB = inertiaWeight(ref.bodyB, axis);
    const float weightSum = weightA + weightB;
    if (weightSum <= kEpsilon) {
        return;
    }
    rotateJointBody(ref.bodyB, glm::angleAxis(angle * strength * (weightB / weightSum), axis));
    rotateJointBody(ref.bodyA, glm::angleAxis(-angle * strength * (weightA / weightSum), axis));
}

namespace {

// World matrix of `entity`'s parent chain, composed from the
// CURRENT TransformComponents root-first. Called while parented
// bodies are mid-conversion/restore, where body ancestors already
// hold world placement in their transforms and non-body ancestors
// still hold local placement — both compose correctly.
glm::mat4 ancestorWorldMatrix(engine::Scene::Registry& registry, entt::entity entity) {
    std::vector<entt::entity> chain;
    entt::entity current = entity;
    for (int depth = 0; depth < 1024; ++depth) {
        const engine::ParentComponent* parent = registry.try_get<engine::ParentComponent>(current);
        if (parent == nullptr || parent->parent == entt::null || !registry.valid(parent->parent)) {
            break;
        }
        current = parent->parent;
        chain.push_back(current);
    }
    glm::mat4 world{1.0f};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (const engine::TransformComponent* transform =
                registry.try_get<engine::TransformComponent>(*it);
            transform != nullptr) {
            world = world * transform->modelMatrix();
        }
    }
    return world;
}

int hierarchyDepth(engine::Scene::Registry& registry, entt::entity entity) {
    int depth = 0;
    entt::entity current = entity;
    for (int guard = 0; guard < 1024; ++guard) {
        const engine::ParentComponent* parent = registry.try_get<engine::ParentComponent>(current);
        if (parent == nullptr || parent->parent == entt::null || !registry.valid(parent->parent)) {
            break;
        }
        current = parent->parent;
        ++depth;
    }
    return depth;
}

} // namespace

void PhysicsSystem::convertParentedBodiesToWorld(Scene& scene) {
    m_parentedBodies.clear();
    auto& registry = scene.registry();
    const auto view = registry.view<TransformComponent, RigidBodyComponent, ParentComponent>();
    std::vector<std::pair<int, entt::entity>> byDepth;
    for (const entt::entity entity : view) {
        byDepth.emplace_back(hierarchyDepth(registry, entity), entity);
    }
    std::sort(byDepth.begin(), byDepth.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& [depth, entity] : byDepth) {
        (void)depth;
        TransformComponent& transform = registry.get<TransformComponent>(entity);
        const glm::mat4 world = ancestorWorldMatrix(registry, entity) * transform.modelMatrix();
        glm::vec3 position;
        glm::vec3 scale;
        glm::quat orientation;
        TransformSystem::decompose(world, position, orientation, scale);
        transform.position = position;
        transform.scale = scale;
        transform.setOrientation(orientation);
        m_parentedBodies.push_back(entity);
    }
}

void PhysicsSystem::restoreParentedBodiesToLocal(Scene& scene) {
    auto& registry = scene.registry();
    // Reverse depth order: a child is re-localized while its body
    // ancestors still hold world placement, so its parent's world
    // matrix composes correctly.
    for (auto it = m_parentedBodies.rbegin(); it != m_parentedBodies.rend(); ++it) {
        if (!registry.valid(*it)) {
            continue;
        }
        TransformComponent* transform = registry.try_get<TransformComponent>(*it);
        if (transform == nullptr) {
            continue;
        }
        const glm::mat4 local = glm::inverse(ancestorWorldMatrix(registry, *it)) * transform->modelMatrix();
        glm::vec3 position;
        glm::vec3 scale;
        glm::quat orientation;
        TransformSystem::decompose(local, position, orientation, scale);
        transform->position = position;
        transform->scale = scale;
        transform->setOrientation(orientation);
    }
    m_parentedBodies.clear();
}

void PhysicsSystem::publishPhysicsEvents(Scene& scene) {
    // Trigger crossings: diff the final sub-step's trigger pair set
    // against the previous update's set.
    {
        const std::unordered_set<uint64_t> current(m_stepTriggerPairs.begin(), m_stepTriggerPairs.end());
        if (m_eventBus != nullptr) {
            auto& registry = scene.registry();
            const auto publishTransition = [this, &registry](uint64_t key, bool entered) {
                const entt::entity first = static_cast<entt::entity>(static_cast<uint32_t>(key >> 32u));
                const entt::entity second = static_cast<entt::entity>(static_cast<uint32_t>(key & 0xFFFFFFFFu));
                // The trigger is whichever side carries a trigger
                // collider (if both do, the pair is reported from the
                // first entity's perspective).
                const ColliderComponent* firstCollider = registry.try_get<ColliderComponent>(first);
                const bool firstIsTrigger = firstCollider != nullptr && firstCollider->isTrigger;
                TriggerEvent event;
                event.triggerEntity = firstIsTrigger ? first : second;
                event.otherEntity = firstIsTrigger ? second : first;
                event.entered = entered;
                m_eventBus->publish(event);
            };
            for (const uint64_t key : current) {
                if (m_activeTriggerPairs.count(key) == 0) {
                    publishTransition(key, true);
                }
            }
            for (const uint64_t key : m_activeTriggerPairs) {
                if (current.count(key) == 0) {
                    publishTransition(key, false);
                }
            }
        }
        m_activeTriggerPairs = current;
    }

    // Collision began: pairs that have contacts now but had none at
    // the previous update. The event carries the pair's strongest
    // contact (largest accumulated normal impulse).
    {
        std::unordered_map<uint64_t, const Contact*> strongest;
        for (const Contact& contact : m_contacts) {
            const uint32_t entityA = entt::to_integral(m_bodies[contact.bodyA].entity);
            const uint32_t entityB = entt::to_integral(m_bodies[contact.bodyB].entity);
            const uint64_t key = (static_cast<uint64_t>(std::min(entityA, entityB)) << 32u) |
                                 static_cast<uint64_t>(std::max(entityA, entityB));
            const auto found = strongest.find(key);
            if (found == strongest.end()) {
                strongest.emplace(key, &contact);
            } else if (contact.normalImpulse > found->second->normalImpulse) {
                found->second = &contact;
            }
        }
        if (m_eventBus != nullptr) {
            for (const auto& [key, contact] : strongest) {
                if (m_activeContactPairs.count(key) == 0) {
                    CollisionEvent event;
                    event.entityA = m_bodies[contact->bodyA].entity;
                    event.entityB = m_bodies[contact->bodyB].entity;
                    event.point = contact->point;
                    event.normal = contact->normal;
                    event.impulse = contact->normalImpulse;
                    m_eventBus->publish(event);
                }
            }
        }
        m_activeContactPairs.clear();
        for (const auto& [key, contact] : strongest) {
            m_activeContactPairs.insert(key);
        }
    }
}

} // namespace engine
