#include "Scene/Systems/PhysicsSystem.h"

#include "Core/Jobs/JobSystem.h"
#include "Scene/Scene.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

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

    snapshotBodies(scene);
    m_lastStats.bodies = m_bodies.size();
    for (const BodyRef& ref : m_bodies) {
        if (ref.body->dynamic()) {
            ++m_lastStats.dynamicBodies;
        }
    }

    integrate(fixedDeltaSeconds, settings, jobs);
    buildGeometry(jobs, settings.parallelMinBodies);
    broadPhase();
    narrowPhase(jobs, settings.parallelMinBodies);
    solveContacts(settings);

    m_lastStats.candidatePairs = m_candidatePairs.size();
    m_lastStats.contacts = m_contacts.size();
    m_lastStats.solverIterations = settings.solverIterations;
    return m_lastStats;
}

void PhysicsSystem::snapshotBodies(Scene& scene) {
    m_bodies.clear();
    auto view = scene.registry().view<TransformComponent, RigidBodyComponent, ColliderComponent>();
    m_bodies.reserve(view.size_hint());
    for (const entt::entity entity : view) {
        m_bodies.push_back(BodyRef{&view.get<TransformComponent>(entity), &view.get<RigidBodyComponent>(entity),
                                   &view.get<ColliderComponent>(entity)});
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
                body.clearForces();
                continue;
            }

            body.inverseMass = 1.0f / body.mass;
            body.acceleration = (body.useGravity ? settings.gravity : glm::vec3{0.0f}) +
                                body.force * body.inverseMass;
            body.velocity += body.acceleration * dt;
            if (settings.linearDamping > 0.0f) {
                body.velocity *= std::max(0.0f, 1.0f - settings.linearDamping * dt);
            }
            // Semi-implicit Euler: the new velocity moves the body. This
            // is stable at the engine's fixed step and conserves the
            // contact solver's assumptions much better than explicit Euler.
            ref.transform->position += body.velocity * dt;
            body.clearForces();
        }
    };

    if (jobs != nullptr && m_bodies.size() >= settings.parallelMinBodies) {
        jobs->parallelFor(m_bodies.size(), 0, integrateRange);
    } else {
        integrateRange(0, m_bodies.size());
    }
}

void PhysicsSystem::buildGeometry(JobSystem* jobs, uint32_t parallelMinBodies) {
    m_geometry.resize(m_bodies.size());
    const auto buildRange = [this](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const BodyRef& ref = m_bodies[i];
            BodyGeometry& geometry = m_geometry[i];
            const glm::vec3 scale = glm::abs(ref.transform->scale);
            geometry.center = ref.transform->position;
            geometry.isPlane = ref.collider->type == ColliderComponent::Type::Plane;

            if (ref.collider->type == ColliderComponent::Type::Sphere) {
                geometry.radius = ref.collider->radius * std::max({scale.x, scale.y, scale.z});
                geometry.halfExtents = glm::vec3{geometry.radius};
            } else {
                geometry.radius = ref.collider->radius * std::max({scale.x, scale.y, scale.z});
                geometry.halfExtents = ref.collider->halfExtents * scale;
            }

            const float normalLengthSq = glm::dot(ref.collider->planeNormal, ref.collider->planeNormal);
            geometry.planeNormal = normalLengthSq > kEpsilon
                                       ? ref.collider->planeNormal / std::sqrt(normalLengthSq)
                                       : glm::vec3{0.0f, 1.0f, 0.0f};
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
            m_broadEntries.push_back(BroadEntry{m_geometry[i].center.x - m_geometry[i].halfExtents.x,
                                                m_geometry[i].center.x + m_geometry[i].halfExtents.x, index});
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
        float penetration = 0.0f;
        if (m_bodies[otherIndex].collider->type == ColliderComponent::Type::Sphere) {
            penetration = other.radius - signedDistance;
        } else {
            const float supportRadius = glm::dot(glm::abs(plane.planeNormal), other.halfExtents);
            penetration = supportRadius - signedDistance;
        }
        if (penetration <= 0.0f) {
            return std::nullopt;
        }
        contact.normal = (planeIndex == bodyA) ? plane.planeNormal : -plane.planeNormal;
        contact.penetration = penetration;
        return contact;
    }

    const bool aIsSphere = m_bodies[bodyA].collider->type == ColliderComponent::Type::Sphere;
    const bool bIsSphere = m_bodies[bodyB].collider->type == ColliderComponent::Type::Sphere;

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
        return contact;
    }

    if (!aIsSphere && !bIsSphere) {
        const glm::vec3 delta = b.center - a.center;
        const glm::vec3 overlap = (a.halfExtents + b.halfExtents) - glm::abs(delta);
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
        return contact;
    }

    // Sphere/box: work out the direction that separates the sphere from
    // the box, then orient the A->B contact normal from that direction.
    const uint32_t sphereIndex = aIsSphere ? bodyA : bodyB;
    const uint32_t boxIndex = aIsSphere ? bodyB : bodyA;
    const BodyGeometry& sphere = m_geometry[sphereIndex];
    const BodyGeometry& box = m_geometry[boxIndex];
    const glm::vec3 boxMin = box.center - box.halfExtents;
    const glm::vec3 boxMax = box.center + box.halfExtents;
    const glm::vec3 closest = glm::clamp(sphere.center, boxMin, boxMax);
    const glm::vec3 delta = sphere.center - closest;
    const float distanceSq = glm::dot(delta, delta);

    glm::vec3 separationDirection;
    if (distanceSq > kEpsilon) {
        const float distance = std::sqrt(distanceSq);
        if (distance >= sphere.radius) {
            return std::nullopt;
        }
        separationDirection = delta / distance;
        contact.penetration = sphere.radius - distance;
    } else {
        // Sphere center is inside the box: leave through the nearest face.
        const std::array<std::pair<float, glm::vec3>, 6> faces = {{
            {sphere.center.x - boxMin.x, glm::vec3{-1.0f, 0.0f, 0.0f}},
            {boxMax.x - sphere.center.x, glm::vec3{1.0f, 0.0f, 0.0f}},
            {sphere.center.y - boxMin.y, glm::vec3{0.0f, -1.0f, 0.0f}},
            {boxMax.y - sphere.center.y, glm::vec3{0.0f, 1.0f, 0.0f}},
            {sphere.center.z - boxMin.z, glm::vec3{0.0f, 0.0f, -1.0f}},
            {boxMax.z - sphere.center.z, glm::vec3{0.0f, 0.0f, 1.0f}},
        }};
        const auto nearest = std::min_element(faces.begin(), faces.end(),
                                              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        separationDirection = nearest->second;
        contact.penetration = sphere.radius + nearest->first;
    }
    contact.normal = aIsSphere ? -separationDirection : separationDirection;
    return contact;
}

float PhysicsSystem::inverseMass(uint32_t bodyIndex) const {
    const RigidBodyComponent& body = *m_bodies[bodyIndex].body;
    return body.dynamic() ? 1.0f / body.mass : 0.0f;
}

void PhysicsSystem::solveContacts(const PhysicsSettings& settings) {
    for (uint32_t iteration = 0; iteration < settings.solverIterations; ++iteration) {
        for (const Contact& contact : m_contacts) {
            resolveVelocity(contact, settings);
        }
    }
    for (const Contact& contact : m_contacts) {
        resolvePosition(contact, settings);
    }
}

void PhysicsSystem::resolveVelocity(const Contact& contact, const PhysicsSettings& settings) {
    const float invA = inverseMass(contact.bodyA);
    const float invB = inverseMass(contact.bodyB);
    const float inverseMassSum = invA + invB;
    if (inverseMassSum <= 0.0f) {
        return;
    }

    RigidBodyComponent& a = *m_bodies[contact.bodyA].body;
    RigidBodyComponent& b = *m_bodies[contact.bodyB].body;
    const glm::vec3 normal = contact.normal;

    float normalImpulse = 0.0f;
    glm::vec3 relativeVelocity = b.velocity - a.velocity;
    const float velocityAlongNormal = glm::dot(relativeVelocity, normal);
    if (velocityAlongNormal < 0.0f) {
        // Slow impacts are resting contacts: no restitution, which keeps
        // stacks from micro-bouncing under gravity every fixed step.
        const float restitution = -velocityAlongNormal >= settings.restitutionVelocityThreshold
                                      ? contact.restitution
                                      : 0.0f;
        normalImpulse = -(1.0f + restitution) * velocityAlongNormal / inverseMassSum;
        a.velocity -= normal * (normalImpulse * invA);
        b.velocity += normal * (normalImpulse * invB);
    }

    if (normalImpulse <= 0.0f || contact.friction <= 0.0f) {
        return;
    }
    relativeVelocity = b.velocity - a.velocity;
    const glm::vec3 tangentialVelocity = relativeVelocity - normal * glm::dot(relativeVelocity, normal);
    const float tangentialSpeed = glm::length(tangentialVelocity);
    if (tangentialSpeed <= kEpsilon) {
        return;
    }
    const glm::vec3 tangent = tangentialVelocity / tangentialSpeed;
    float frictionImpulse = -glm::dot(relativeVelocity, tangent) / inverseMassSum;
    const float maxFriction = normalImpulse * contact.friction;
    frictionImpulse = std::clamp(frictionImpulse, -maxFriction, maxFriction);
    a.velocity -= tangent * (frictionImpulse * invA);
    b.velocity += tangent * (frictionImpulse * invB);
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

} // namespace engine
