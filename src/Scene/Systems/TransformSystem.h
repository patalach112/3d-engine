#pragma once
#include "Core/Export.h"

#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace engine {

class Scene;
struct TransformComponent;
struct WorldTransformComponent;

// ---------------------------------------------------------------------------
// TransformSystem (Phase 18)
//
// Recomputes WorldTransformComponent for every entity carrying a
// TransformComponent by walking the Parent/Children tree from the
// roots down:
//
//   worldMatrix(entity) = worldMatrix(parent) * modelMatrix(entity)
//
// with decomposed world position/orientation/scale alongside.
// Orientation composes as quaternions, so chains never hit gimbal
// lock; non-uniform scale under rotation composes into the matrix
// (the decomposed world scale is then the per-axis column length —
// what consumers of the matrix already assume).
//
// The pass is a full recompute: local transforms are plain data
// mutated directly by gameplay and physics (no setters to hang
// dirty flags on), and at scene-graph scale the walk is cheap.
// Entities in a malformed hierarchy (a parent cycle creatable only
// by patching components directly, bypassing Scene::setParent)
// fall back to their local matrix so one bad chain can never stall
// the pass. Entities whose parent has no transform are treated as
// roots of their subtree.
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API TransformSystem {
public:
    // Returns how many entities received a (re)computed world transform.
    static size_t update(Scene& scene);

    // The world matrix a consumer should use: the propagated one
    // when available, the local model matrix otherwise (entities
    // are correct before the first TransformSystem pass runs).
    [[nodiscard]] static glm::mat4 effectiveWorldMatrix(const TransformComponent& transform,
                                                        const WorldTransformComponent* world);

    // Splits a TRS matrix into position / orientation / scale
    // (shear, when present, is dropped — the standard approximation
    // for re-localizing a transform under a rotated, non-uniformly
    // scaled parent).
    static void decompose(const glm::mat4& matrix, glm::vec3& outPosition, glm::quat& outOrientation,
                          glm::vec3& outScale);
};

} // namespace engine
