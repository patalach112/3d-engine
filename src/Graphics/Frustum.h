#pragma once

#include "Geometry/Bounds.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace engine {

// Half-space plane in Hessian form: points p with dot(normal, p) + offset
// >= 0 are on the "inside". Normals are normalized on construction.
struct Plane {
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float offset = 0.0f;

    Plane() = default;
    explicit Plane(const glm::vec4& coefficients);

    [[nodiscard]] float signedDistance(const glm::vec3& point) const {
        return glm::dot(normal, point) + offset;
    }
};

// ---------------------------------------------------------------------------
// Frustum — the camera's view volume, extracted from a view-projection
// matrix with the Gribb–Hartmann method. The near plane uses Vulkan's
// [0, w] clip-space depth (row 2 alone, not row 3 + row 2 as in OpenGL).
//
// Intersection tests use the p-vertex method: only the box corner farthest
// along each plane normal can keep the box inside, which makes AABB tests
// both exact and cheap.
// ---------------------------------------------------------------------------
class Frustum {
public:
    explicit Frustum(const glm::mat4& viewProjection);

    [[nodiscard]] bool intersects(const AABB& box) const;
    [[nodiscard]] bool intersectsSphere(const glm::vec3& center, float radius) const;
    [[nodiscard]] const std::array<Plane, 6>& planes() const { return m_planes; }

private:
    std::array<Plane, 6> m_planes; // left, right, bottom, top, near, far
};

} // namespace engine
