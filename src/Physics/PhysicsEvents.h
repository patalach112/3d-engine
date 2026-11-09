#pragma once

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

namespace engine {

// ---------------------------------------------------------------------------
// Physics events (Phase 17)
//
// Published by PhysicsSystem on its EventBus (see
// PhysicsSystem::setEventBus) at the END of an update, after every
// sub-step has been solved — never from inside the solver — so
// listeners may freely inspect or modify the scene.
// ---------------------------------------------------------------------------

// A contact manifold appeared for a pair that had no contacts at the
// end of the previous update: a new impact or a new resting contact.
// Pairs that stay in contact do not re-fire; a pair that separates
// and collides again fires again.
struct CollisionEvent {
    entt::entity entityA = entt::null;
    entt::entity entityB = entt::null;
    glm::vec3 point{0.0f};        // world-space point of the pair's strongest contact
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; // contact normal as solved (entityA side toward entityB)
    float impulse = 0.0f;         // accumulated normal impulse of that contact (N·s)
};

// A body started (entered=true) or stopped (entered=false)
// overlapping a trigger volume. Triggers are colliders with
// ColliderComponent::isTrigger set: they generate these events
// instead of contact response, and only for pairs that pass the
// usual layer/mask filter.
struct TriggerEvent {
    entt::entity triggerEntity = entt::null; // the entity whose collider is the trigger
    entt::entity otherEntity = entt::null;   // the entity that crossed it
    bool entered = true;
};

} // namespace engine
