#include "Scripting/StalkerAISystem.h"

#include "Geometry/Ray.h"
#include "Scene/Components.h"
#include "Scene/Raycast.h"
#include "Scene/Scene.h"
#include "Scripting/DSLComponents.h"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>

namespace engine {

namespace {

// Pure seek steering (shared by both registry flavours): velocity
// toward the target, capped at moveSpeed, zero inside the stopping
// distance.
[[nodiscard]] glm::vec3 seekVelocity(const glm::vec3& selfPos, const glm::vec3& targetPos, const AIBehaviorComponent& ai) {
    const glm::vec3 toTarget = targetPos - selfPos;
    const float distance = glm::length(toTarget);
    if (distance <= ai.stoppingDistance || distance <= 1e-5f) {
        return glm::vec3{0.0f};
    }
    return (toTarget / distance) * ai.moveSpeed;
}

// Raycast obstacle avoidance: three feelers (forward, ±35°) probe
// for obstacles inside the avoidance radius; a hit bends the
// velocity away along the surface normal and projects out the
// into-wall component so the stalker slides along walls instead
// of sticking to them.
void applyAvoidance(Scene& scene, entt::entity self, entt::entity target, const glm::vec3& selfPos,
                    const AIBehaviorComponent& ai, glm::vec3& velocity) {
    if (glm::length(velocity) <= 1e-5f) {
        return;
    }
    const glm::vec3 forward = glm::normalize(velocity);
    const glm::vec3 origin = selfPos + glm::vec3{0.0f, 0.4f, 0.0f};
    const float cosA = std::cos(glm::radians(35.0f));
    const float sinA = std::sin(glm::radians(35.0f));
    const glm::vec3 feelers[3] = {
        forward,
        glm::vec3{forward.x * cosA - forward.z * sinA, 0.0f, forward.x * sinA + forward.z * cosA},
        glm::vec3{forward.x * cosA + forward.z * sinA, 0.0f, -forward.x * sinA + forward.z * cosA},
    };
    for (const glm::vec3& feelerDir : feelers) {
        Ray ray;
        ray.origin = origin;
        ray.direction = feelerDir;
        RaycastHit hit;
        if (scene.raycast(ray, hit) && hit.entity != self && hit.entity != target && hit.distance < ai.avoidanceRadius &&
            hit.distance > 0.0f) {
            const float strength = (1.0f - hit.distance / ai.avoidanceRadius) * ai.moveSpeed * 1.6f;
            velocity += hit.normal * strength;
            const float into = glm::dot(velocity, hit.normal); // wall slide
            if (into < 0.0f) {
                velocity -= hit.normal * into;
            }
        }
    }
    const float speed = glm::length(velocity);
    if (speed > ai.moveSpeed && speed > 1e-5f) {
        velocity = (velocity / speed) * ai.moveSpeed;
    }
}

void integrate(TransformComponent& transform, AIBehaviorComponent& ai, const glm::vec3& velocity, float dt) {
    ai.steering = velocity;
    if (glm::length(velocity) > 1e-5f) {
        transform.position += velocity * dt;
        transform.rotationDegrees.y = glm::degrees(std::atan2(velocity.x, velocity.z));
        transform.syncOrientationFromEuler();
    }
}

template <typename Registry>
[[nodiscard]] entt::entity findTagged(Registry& registry, const char* name) {
    entt::entity found = entt::null;
    auto view = registry.template view<TagComponent>();
    view.each([&](entt::entity e, const TagComponent& tag) {
        if (found == entt::null && tag.tag == name) {
            found = e;
        }
    });
    return found;
}

} // namespace

void StalkerAISystem::update(Scene& scene, float deltaSeconds) {
    auto& registry = scene.registry();
    auto view = registry.view<AIBehaviorComponent, TransformComponent>();
    view.each([&](entt::entity e, AIBehaviorComponent& ai, TransformComponent& transform) {
        if (!ai.active || ai.behavior != AIBehavior::Stalker || registry.all_of<FrozenComponent>(e)) {
            ai.steering = glm::vec3{0.0f};
            return;
        }
        entt::entity target = ai.target;
        if (target == entt::null || !registry.valid(target) || target == e) {
            target = findTagged(registry, "Player");
        }
        if (target == entt::null || target == e || !registry.all_of<TransformComponent>(target)) {
            ai.steering = glm::vec3{0.0f};
            return;
        }
        glm::vec3 velocity = seekVelocity(transform.position, registry.get<TransformComponent>(target).position, ai);
        applyAvoidance(scene, e, target, transform.position, ai, velocity);
        integrate(transform, ai, velocity, deltaSeconds);
    });
}

void StalkerAISystem::update(entt::registry& registry, float deltaSeconds) {
    auto view = registry.view<AIBehaviorComponent, TransformComponent>();
    view.each([&](entt::entity e, AIBehaviorComponent& ai, TransformComponent& transform) {
        if (!ai.active || ai.behavior != AIBehavior::Stalker || registry.all_of<FrozenComponent>(e)) {
            ai.steering = glm::vec3{0.0f};
            return;
        }
        entt::entity target = ai.target;
        if (target == entt::null || !registry.valid(target) || target == e) {
            target = findTagged(registry, "Player");
        }
        if (target == entt::null || target == e || !registry.all_of<TransformComponent>(target)) {
            ai.steering = glm::vec3{0.0f};
            return;
        }
        integrate(transform, ai, seekVelocity(transform.position, registry.get<TransformComponent>(target).position, ai),
                  deltaSeconds);
    });
}

} // namespace engine
