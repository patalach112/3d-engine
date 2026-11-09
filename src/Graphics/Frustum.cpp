#include "Graphics/Frustum.h"

#include <glm/geometric.hpp>

namespace engine {
namespace {

// Row i of a column-major 4x4 matrix as a vec4.
glm::vec4 row(const glm::mat4& matrix, int index) {
    return {matrix[0][index], matrix[1][index], matrix[2][index], matrix[3][index]};
}

} // namespace

Plane::Plane(const glm::vec4& coefficients) {
    const glm::vec3 unnormalized{coefficients};
    const float length = glm::length(unnormalized);
    if (length > 0.0f) {
        normal = unnormalized / length;
        offset = coefficients.w / length;
    }
}

Frustum::Frustum(const glm::mat4& viewProjection) {
    const glm::vec4 row0 = row(viewProjection, 0);
    const glm::vec4 row1 = row(viewProjection, 1);
    const glm::vec4 row2 = row(viewProjection, 2);
    const glm::vec4 row3 = row(viewProjection, 3);

    m_planes = {
        Plane{row3 + row0}, // left:   x >= -w
        Plane{row3 - row0}, // right:  x <=  w
        Plane{row3 + row1}, // bottom: y >= -w
        Plane{row3 - row1}, // top:    y <=  w
        Plane{row2},        // near:   z >= 0   (Vulkan clip space)
        Plane{row3 - row2}, // far:    z <=  w
    };
}

bool Frustum::intersects(const AABB& box) const {
    for (const Plane& plane : m_planes) {
        // p-vertex: the corner most aligned with the plane normal. If even
        // it is outside, the whole box is outside this plane.
        const glm::vec3 pVertex{plane.normal.x >= 0.0f ? box.max.x : box.min.x,
                                plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                                plane.normal.z >= 0.0f ? box.max.z : box.min.z};
        if (plane.signedDistance(pVertex) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersectsSphere(const glm::vec3& center, float radius) const {
    for (const Plane& plane : m_planes) {
        if (plane.signedDistance(center) < -radius) {
            return false;
        }
    }
    return true;
}

} // namespace engine
