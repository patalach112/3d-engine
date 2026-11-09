#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include "Scene/Systems/PhysicsSystem.h"

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
#include <utility>

namespace engine {
namespace {

constexpr float kEpsilon = 1e-6f;

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
    return glm::mat3{factor * (dimensions.y * dimensions.y + dimensions.z * dimensions.z), 0.0f, 0.0f,
                     0.0f, factor * (dimensions.x * dimensions.x + dimensions.z * dimensions.z), 0.0f,
                     0.0f, 0.0f, factor * (dimensions.x * dimensions.x + dimensions.y * dimensions.y)};
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

    m_ccdEnabled = settings.ccdEnabled;
    snapshotBodies(scene);
    snapshotConstraints(scene);
    if (settings.gravitationalParameter > 0.0f) {
        m_gravitySources.push_back(GravitySource{settings.gravityCenter, settings.gravitationalParameter, 0.0f});
    }
    applyExternalForces();
    m_lastStats.bodies = m_bodies.size();
    for (const BodyRef& ref : m_bodies) {
        if (ref.body->dynamic()) {
            ++m_lastStats.dynamicBodies;
        }
    }

    integrate(fixedDeltaSeconds, settings, jobs);
    buildGeometry(jobs, settings.parallelMinBodies, static_cast<float>(fixedDeltaSeconds));
    broadPhase();
    narrowPhase(jobs, settings.parallelMinBodies);
    applyContinuousCollisionStops();
    buildIslands();
    solveConstraints(settings, jobs);

    m_lastStats.candidatePairs = m_candidatePairs.size();
    m_lastStats.contacts = m_contacts.size();
    m_lastStats.joints = m_joints.size();
    m_lastStats.solverIterations = settings.solverIterations;
    return m_lastStats;
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
        transform.scale = state.scale;
        body.velocity = state.velocity;
        body.angularVelocity = state.angularVelocity;
        body.angularMomentum = state.angularMomentum;
        body.orientation = state.orientation;
        body.orientationInitialized = true;
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
        ref.restLength = joint.restLength >= 0.0f ? joint.restLength : glm::length(pointB - pointA);
        m_joints.push_back(ref);
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
            if (!body.dynamic()) {
                body.inverseMass = 0.0f;
                body.acceleration = glm::vec3{0.0f};
                body.clearForcesAndTorques();
                continue;
            }
            initializeOrientationAndInertia(*ref.transform, body, *ref.collider);

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
            angularVelocity = inverseInertiaWorld(static_cast<uint32_t>(i), body.angularMomentum);

            const float angularSpeed = glm::length(angularVelocity);
            if (angularSpeed > kEpsilon) {
                const glm::vec3 axis = angularVelocity / angularSpeed;
                const glm::quat delta = glm::angleAxis(angularSpeed * dt, axis);
                body.orientation = glm::normalize(delta * body.orientation);
            }
            body.angularVelocity = glm::degrees(angularVelocity);
            ref.transform->position += body.velocity * dt;
            ref.transform->rotationDegrees =
                glm::mod(glm::degrees(glm::eulerAngles(body.orientation)), 360.0f);
            body.clearForcesAndTorques();
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
            geometry.center = ref.transform->position;
            geometry.previousCenter = ref.transform->position - body.velocity * dt;
            geometry.displacement = body.velocity * dt;
            geometry.isPlane = collider.type == ColliderComponent::Type::Plane;
            geometry.axes = rotationMatrix(body.orientation);
            geometry.boundsCenter = geometry.center;

            const glm::vec3 scale = glm::abs(ref.transform->scale);
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
    m_broadEntries.clear();
    m_planeBodies.clear();
    m_candidatePairs.clear();
    m_broadEntries.reserve(m_bodies.size());

    for (size_t i = 0; i < m_bodies.size(); ++i) {
        const uint32_t index = static_cast<uint32_t>(i);
        if (m_geometry[i].isPlane) {
            m_planeBodies.push_back(index);
        } else {
            const BodyGeometry& geometry = m_geometry[i];
            // Expand the swept interval by the body's own step motion so a
            // fast body remains a candidate against obstacles it crossed
            // during this step, even if it no longer overlaps discretely.
            const float pad = glm::length(geometry.displacement) * 2.0f;
            m_broadEntries.push_back(BroadEntry{geometry.boundsCenter.x - geometry.boundsHalf.x - pad,
                                                geometry.boundsCenter.x + geometry.boundsHalf.x + pad, index});
        }
    }

    std::sort(m_broadEntries.begin(), m_broadEntries.end(), [](const BroadEntry& a, const BroadEntry& b) {
        return a.minX != b.minX ? a.minX < b.minX : a.bodyIndex < b.bodyIndex;
    });

    // Sweep on X and keep only bodies whose intervals still overlap.
    // Full AABB rejection happens in the narrow phase; this pass turns a
    // naive O(n^2) search into sort + active-window work for scenes whose
    // bodies are spatially distributed.
    std::vector<BroadEntry> active;
    active.reserve(m_broadEntries.size());
    for (const BroadEntry& entry : m_broadEntries) {
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&entry](const BroadEntry& activeEntry) {
                                        return activeEntry.maxX < entry.minX;
                                    }),
                     active.end());
        for (const BroadEntry& activeEntry : active) {
            m_candidatePairs.emplace_back(std::min(activeEntry.bodyIndex, entry.bodyIndex),
                                          std::max(activeEntry.bodyIndex, entry.bodyIndex));
        }
        active.push_back(entry);
    }

    for (const uint32_t planeBody : m_planeBodies) {
        for (size_t i = 0; i < m_bodies.size(); ++i) {
            const uint32_t other = static_cast<uint32_t>(i);
            if (other == planeBody || m_geometry[other].isPlane) {
                continue;
            }
            m_candidatePairs.emplace_back(std::min(planeBody, other), std::max(planeBody, other));
        }
    }

    std::sort(m_candidatePairs.begin(), m_candidatePairs.end());
    m_candidatePairs.erase(std::unique(m_candidatePairs.begin(), m_candidatePairs.end()), m_candidatePairs.end());
}

void PhysicsSystem::narrowPhase(JobSystem* jobs, uint32_t parallelMinBodies) {
    m_contactSlots.assign(m_candidatePairs.size(), std::nullopt);
    const auto evaluateRange = [this](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const auto [bodyA, bodyB] = m_candidatePairs[i];
            m_contactSlots[i] = collide(bodyA, bodyB);
            if (!m_contactSlots[i].has_value() && m_ccdEnabled) {
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
                    m_contactSlots[i] = sweptCollide(bodyA, bodyB, base);
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
    for (const std::optional<Contact>& contact : m_contactSlots) {
        if (contact.has_value()) {
            m_contacts.push_back(*contact);
        }
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

        const glm::vec3 clamped = glm::clamp(other.center, other.center - other.boundsHalf, other.center + other.boundsHalf);
        (void)clamped;
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
    for (Contact& contact : m_contacts) {
        const uint64_t key = (static_cast<uint64_t>(contact.bodyA) << 32u) | contact.bodyB;
        for (const WarmContact& warm : m_warmContacts) {
            if (warm.pairKey == key && glm::dot(warm.normal, contact.normal) > 0.5f) {
                contact.normalImpulse = warm.normalImpulse;
                contact.tangentImpulse = warm.tangentImpulse;
                applyWarmStart(contact);
                break;
            }
        }
    }

    for (Contact& contact : m_contacts) {
        const glm::vec3 relativeVelocity =
            m_bodies[contact.bodyB].body->velocity - m_bodies[contact.bodyA].body->velocity;
        const float vn = glm::dot(relativeVelocity, contact.normal);
        contact.restitutionTarget = vn < -settings.restitutionVelocityThreshold ? -contact.restitution * vn : 0.0f;
    }
}

void PhysicsSystem::storeWarmStarting() {
    m_warmContacts.clear();
    m_warmContacts.reserve(m_contacts.size());
    for (const Contact& contact : m_contacts) {
        WarmContact warm;
        warm.pairKey = (static_cast<uint64_t>(contact.bodyA) << 32u) | contact.bodyB;
        warm.normal = contact.normal;
        warm.normalImpulse = contact.normalImpulse;
        warm.tangentImpulse = contact.tangentImpulse;
        m_warmContacts.push_back(warm);
    }
}

void PhysicsSystem::applyWarmStart(Contact& contact) {
    const float invA = inverseMass(contact.bodyA);
    const float invB = inverseMass(contact.bodyB);
    if (invA + invB <= 0.0f) {
        return;
    }
    RigidBodyComponent& a = *m_bodies[contact.bodyA].body;
    RigidBodyComponent& b = *m_bodies[contact.bodyB].body;
    const glm::vec3 impulse = contact.normal * contact.normalImpulse + contact.tangentImpulse;
    a.velocity -= impulse * invA;
    b.velocity += impulse * invB;
}

void PhysicsSystem::applyContinuousCollisionStops() {
    for (const Contact& contact : m_contacts) {
        if (!contact.continuous || contact.timeOfImpact >= 1.0f) {
            continue;
        }
        const float invA = inverseMass(contact.bodyA);
        const float invB = inverseMass(contact.bodyB);
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

void PhysicsSystem::solveConstraints(const PhysicsSettings& settings, JobSystem* jobs) {
    prepareWarmStarting(settings);
    const auto solveOneIsland = [this, &settings](SolverIsland& island) {
        for (uint32_t iteration = 0; iteration < settings.solverIterations; ++iteration) {
            for (const size_t jointIndex : island.joints) {
                resolveJointVelocity(m_joints[jointIndex]);
            }
            for (const size_t contactIndex : island.contacts) {
                resolveVelocity(m_contacts[contactIndex], settings);
            }
        }
        for (const size_t jointIndex : island.joints) {
            resolveJointPosition(m_joints[jointIndex]);
        }
        for (const size_t contactIndex : island.contacts) {
            resolvePosition(m_contacts[contactIndex], settings);
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
    storeWarmStarting();
}

void PhysicsSystem::resolveVelocity(Contact& contact, const PhysicsSettings& settings) {
    const float invA = inverseMass(contact.bodyA);
    const float invB = inverseMass(contact.bodyB);
    const float inverseMassSum = invA + invB;
    if (inverseMassSum <= 0.0f) {
        return;
    }

    RigidBodyComponent& a = *m_bodies[contact.bodyA].body;
    RigidBodyComponent& b = *m_bodies[contact.bodyB].body;
    const glm::vec3 normal = contact.normal;

    glm::vec3 relativeVelocity = b.velocity - a.velocity;
    const float velocityAlongNormal = glm::dot(relativeVelocity, normal);
    {
        // Accumulated sequential impulse: the total normal impulse is
        // clamped to the compressive half-space, while each iteration
        // applies only the delta. This lets warm-started contacts reduce
        // an excessive stored impulse instead of bouncing the stack.
        const float lambda = (contact.restitutionTarget - velocityAlongNormal) / inverseMassSum;
        const float newImpulse = std::max(0.0f, contact.normalImpulse + lambda);
        const float deltaImpulse = newImpulse - contact.normalImpulse;
        contact.normalImpulse = newImpulse;
        a.velocity -= normal * deltaImpulse * invA;
        b.velocity += normal * deltaImpulse * invB;
    }
    (void)settings;

    if (contact.friction <= 0.0f) {
        return;
    }
    relativeVelocity = b.velocity - a.velocity;
    const glm::vec3 tangentialVelocity = relativeVelocity - normal * glm::dot(relativeVelocity, normal);
    const float tangentialSpeed = glm::length(tangentialVelocity);
    if (tangentialSpeed <= kEpsilon) {
        return;
    }
    const glm::vec3 tangent = tangentialVelocity / tangentialSpeed;
    const glm::vec3 tangent2 = glm::normalize(glm::cross(normal, tangent));
    const float friction1 = contact.anisotropicFriction.x >= 0.0f ? contact.anisotropicFriction.x : contact.friction;
    const float friction2 = contact.anisotropicFriction.y >= 0.0f ? contact.anisotropicFriction.y : contact.friction;
    const glm::vec3 desired = tangent * (-glm::dot(relativeVelocity, tangent) / inverseMassSum) +
                              tangent2 * (-glm::dot(relativeVelocity, tangent2) / inverseMassSum);
    const float maxFriction1 = std::max(contact.normalImpulse, 0.0f) * friction1;
    const float maxFriction2 = std::max(contact.normalImpulse, 0.0f) * friction2;
    glm::vec3 clamped = tangent * std::clamp(glm::dot(desired, tangent), -maxFriction1, maxFriction1) +
                        tangent2 * std::clamp(glm::dot(desired, tangent2), -maxFriction2, maxFriction2);
    const glm::vec3 deltaImpulse = clamped - contact.tangentImpulse;
    contact.tangentImpulse = clamped;
    a.velocity -= deltaImpulse * invA;
    b.velocity += deltaImpulse * invB;
}

void PhysicsSystem::resolvePosition(const Contact& contact, const PhysicsSettings& settings) {
    if (contact.penetration <= settings.contactSlop) {
        return;
    }
    const float invA = inverseMass(contact.bodyA);
    const float invB = inverseMass(contact.bodyB);
    const float inverseMassSum = invA + invB;
    if (inverseMassSum <= 0.0f) {
        return;
    }
    const float correctionMagnitude = (contact.penetration - settings.contactSlop) *
                                      std::clamp(settings.positionCorrection, 0.0f, 1.0f) / inverseMassSum;
    m_bodies[contact.bodyA].transform->position -= contact.normal * (correctionMagnitude * invA);
    m_bodies[contact.bodyB].transform->position += contact.normal * (correctionMagnitude * invB);
}

void PhysicsSystem::resolveJointVelocity(const JointRef& ref) {
    JointComponent& joint = *ref.joint;
    RigidBodyComponent& a = *m_bodies[ref.bodyA].body;
    RigidBodyComponent* b = ref.hasBodyB ? m_bodies[ref.bodyB].body : nullptr;
    const float invA = inverseMass(ref.bodyA);
    const float invB = ref.hasBodyB ? inverseMass(ref.bodyB) : 0.0f;
    const float inverseMassSum = invA + invB;
    if (inverseMassSum <= 0.0f || joint.type == JointType::SpringDamper) {
        return; // spring-dampers are force constraints, applied before integration
    }

    const glm::vec3 pointA = m_bodies[ref.bodyA].transform->position + a.orientation * joint.anchorA;
    glm::vec3 pointB = joint.anchorB;
    if (ref.hasBodyB && b != nullptr) {
        pointB = m_bodies[ref.bodyB].transform->position + b->orientation * joint.anchorB;
    }
    const glm::vec3 axis = pointB - pointA;
    const float distance = glm::length(axis);
    const glm::vec3 direction = distance > kEpsilon ? axis / distance : glm::vec3{1.0f, 0.0f, 0.0f};

    if (joint.type == JointType::Hinge) {
        const glm::vec3 hingeAxis = glm::dot(joint.hingeAxis, joint.hingeAxis) > kEpsilon
                                        ? glm::normalize(joint.hingeAxis)
                                        : glm::vec3{0.0f, 0.0f, 1.0f};
        (void)hingeAxis;
        const float scalarError = distance - ref.restLength;
        const float velocityError = glm::dot((b != nullptr ? b->velocity : glm::vec3{0.0f}) - a.velocity, direction);
        const float impulse = (-velocityError - scalarError * 0.2f) / inverseMassSum;
        a.velocity -= direction * impulse * invA;
        if (b != nullptr) {
            b->velocity += direction * impulse * invB;
        }
        return;
    }

    {
        const int constraintPasses = (joint.type == JointType::Distance) ? 8 : 1;
        for (int pass = 0; pass < constraintPasses; ++pass) {
            const glm::vec3 currentPointA =
                m_bodies[ref.bodyA].transform->position + a.orientation * joint.anchorA;
            glm::vec3 currentPointB = joint.anchorB;
            if (ref.hasBodyB && b != nullptr) {
                currentPointB = m_bodies[ref.bodyB].transform->position + b->orientation * joint.anchorB;
            }
            const glm::vec3 currentAxis = currentPointB - currentPointA;
            const float currentDistance = glm::length(currentAxis);
            const glm::vec3 currentDirection =
                currentDistance > kEpsilon ? currentAxis / currentDistance : glm::vec3{1.0f, 0.0f, 0.0f};
            const float error = currentDistance - ref.restLength;
            const glm::vec3 velocity = (b != nullptr ? b->velocity : glm::vec3{0.0f}) - a.velocity;
            const float velocityError = glm::dot(velocity, currentDirection);
            const float stiffnessFactor = joint.type == JointType::Distance
                                             ? 1.0f
                                             : glm::clamp(joint.stiffness / std::max(1.0f, joint.stiffness), 0.1f, 1.0f);
            const float bias = joint.type == JointType::Distance ? error * 0.8f : error * joint.stiffness * 0.002f;
            const float impulse = (-velocityError - bias) * stiffnessFactor / inverseMassSum;
            a.velocity -= currentDirection * impulse * invA;
            if (b != nullptr) {
                b->velocity += currentDirection * impulse * invB;
            }
        }
    }
}

void PhysicsSystem::resolveJointPosition(const JointRef& ref) {
    JointComponent& joint = *ref.joint;
    RigidBodyComponent& a = *m_bodies[ref.bodyA].body;
    RigidBodyComponent* b = ref.hasBodyB ? m_bodies[ref.bodyB].body : nullptr;
    const float invA = inverseMass(ref.bodyA);
    const float invB = ref.hasBodyB ? inverseMass(ref.bodyB) : 0.0f;
    const float inverseMassSum = invA + invB;
    if (inverseMassSum <= 0.0f) {
        return;
    }
    const glm::vec3 pointA = m_bodies[ref.bodyA].transform->position + a.orientation * joint.anchorA;
    glm::vec3 pointB = joint.anchorB;
    if (ref.hasBodyB && b != nullptr) {
        pointB = m_bodies[ref.bodyB].transform->position + b->orientation * joint.anchorB;
    }
    const glm::vec3 axis = pointB - pointA;
    const float distance = glm::length(axis);
    const float strength = joint.type == JointType::Distance ? 0.35f : 0.05f;
    const glm::vec3 correction = axis * ((distance - ref.restLength) / distance * strength);
    if (invA > 0.0f) {
        m_bodies[ref.bodyA].transform->position += correction * (invA / inverseMassSum);
    }
    if (ref.hasBodyB && invB > 0.0f) {
        m_bodies[ref.bodyB].transform->position -= correction * (invB / inverseMassSum);
    }
}

} // namespace engine
