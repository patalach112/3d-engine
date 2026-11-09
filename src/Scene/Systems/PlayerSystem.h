#pragma once

#include "Core/Export.h"

#include <glm/vec3.hpp>

namespace engine {

class Input;
class Scene;

// ---------------------------------------------------------------------------
// PlayerSystem
//
// Drives the physical first-person player: the active camera entity also
// owns a TransformComponent, a dynamic RigidBodyComponent, and a capsule
// ColliderComponent. This system translates movement input into rigid-body
// velocity before the fixed physics step (so gravity, pushback, friction,
// and sliding are resolved by PhysicsSystem), then copies the simulated
// capsule position back to the camera with a fixed eye offset.
//
// The 1:1 hitbox contract: capsule radius 0.30 m and segment half-height
// 0.45 m (total height 1.50 m), eye 0.50 m above the capsule center. The
// rendered camera therefore occupies exactly the simulated capsule — it
// cannot clip through floors, walls, or cubes that the capsule cannot pass.
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API PlayerSystem {
public:
    static constexpr float kCapsuleRadius = 0.30f;
    static constexpr float kCapsuleHalfHeight = 0.45f;
    static constexpr float kEyeHeight = 0.50f;

    // True when the active camera entity has the full physical-player
    // component set (camera + transform + rigid body + collider).
    [[nodiscard]] static bool hasPhysicalPlayer(const Scene& scene);

    // Converts a local movement direction (x = right, y = vertical thrust,
    // z = forward) into smoothed rigid-body velocity. Horizontal velocity
    // follows the camera yaw basis; vertical velocity is preserved (gravity)
    // unless localDirection.y requests thrust. Call in the fixed update,
    // before PhysicsSystem. Returns false when there is no physical player.
    static bool driveMovement(Scene& scene, glm::vec3 localDirection, double fixedDeltaSeconds,
                              bool boost = false);

    // Input-driven form of driveMovement(): WASD + Space/E/Q + Shift.
    static bool update(Scene& scene, const Input& input, double fixedDeltaSeconds);

    // Copies the simulated capsule center (+ eye offset) into the camera.
    // Call after physics (and from the variable update so the camera tracks
    // the latest fixed-step result even between physics steps).
    static bool syncCamera(Scene& scene, float eyeHeight = kEyeHeight);
};

} // namespace engine
