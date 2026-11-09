#pragma once
#include "Core/Export.h"

#include "Core/Camera.h"
#include "Geometry/Ray.h"

#include <entt/entt.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace engine {

// ---------------------------------------------------------------------------
// RaycastHit
//
// One scene raycast result: the entity that was hit, the world-space
// distance along the ray, and the world-space hit point and surface
// normal (unit length, oriented against the incoming ray). `entity` is
// entt::null on a default-constructed (no-hit) result.
// ---------------------------------------------------------------------------
struct RaycastHit {
    entt::entity entity = entt::null;
    float distance = 0.0f;
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

// Builds the world-space picking ray through the given normalized
// device coordinates (see rayThroughNdc for the convention: y grows
// downward, matching window cursor coordinates via screenToNdc) for
// the given camera and viewport aspect ratio (width / height). The
// ray's origin is the camera's eye position and its direction passes
// exactly through the specified pixel, so hit distances are measured
// from the eye.
[[nodiscard]] ENGINE_SCENE_API Ray cameraRay(const Camera& camera, glm::vec2 ndc, float aspectRatio);

} // namespace engine
