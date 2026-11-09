#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace engine {

// Axis-aligned bounding box. Used as a mesh's local-space bounds and,
// after transformation by a model matrix, as its world-space bounds for
// frustum culling.
struct AABB {
    glm::vec3 min{-0.5f};
    glm::vec3 max{0.5f};

    [[nodiscard]] glm::vec3 center() const { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 extents() const { return (max - min) * 0.5f; }

    // World-space bounds of this box under the given transform: all eight
    // corners are transformed and re-bounded (exact for any affine matrix,
    // including rotation — the result is the tightest AABB around the
    // transformed box).
    [[nodiscard]] AABB transformed(const glm::mat4& matrix) const;
};

} // namespace engine
