#pragma once

#include "Core/Export.h"

#include <entt/entity/entity.hpp>

namespace engine {

class Scene;

// ---------------------------------------------------------------------------
// StalkerAISystem — the native C++ side of the hybrid architecture.
//
// The DSL only says `behavior = stalker`; everything hard lives
// here in native code with direct EnTT access, exactly the split
// the hybrid design calls for ("StalkerAISystem::Update(registry,
// dt)" in the reference architecture): seek the target (the entity
// tagged "Player" unless AIBehaviorComponent::target overrides),
// with raycast-based obstacle avoidance — three feelers (forward,
// ±35°) probe Scene::raycast, any hit inside avoidanceRadius steers
// away from the surface normal, and wall sliding keeps momentum
// along the wall instead of sticking. Frozen entities and
// behaviour-less entities are skipped; the system writes the
// steering result back into AIBehaviorComponent::steering so tools
// can visualise it, and moves the TransformComponent directly
// (kinematic steering; physics bodies still collide via their own
// colliders, the next PhysicsSystem step resolves penetration).
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API StalkerAISystem {
public:
    // Scene form: full behaviour — seek + raycast obstacle avoidance.
    static void update(Scene& scene, float deltaSeconds);
    // Direct-registry form (the native-fallback entry point: native
    // scripts and tests drive EnTT directly). Seek steering only —
    // obstacle avoidance needs Scene raycasts, use the Scene form
    // in the frame loop.
    static void update(entt::registry& registry, float deltaSeconds);
};

} // namespace engine
