#include "Geometry/Bounds.h"

#include <glm/common.hpp>
#include <glm/vec4.hpp>

#include <limits>

namespace engine {

float AABB::surfaceArea() const {
    const glm::vec3 size = max - min;
    return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
}

bool AABB::contains(const AABB& other) const {
    return other.min.x >= min.x && other.min.y >= min.y && other.min.z >= min.z && other.max.x <= max.x &&
           other.max.y <= max.y && other.max.z <= max.z;
}

bool AABB::overlaps(const AABB& other) const {
    return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y && max.y >= other.min.y &&
           min.z <= other.max.z && max.z >= other.min.z;
}

AABB AABB::combined(const AABB& other) const {
    return AABB{glm::min(min, other.min), glm::max(max, other.max)};
}

AABB AABB::transformed(const glm::mat4& matrix) const {
    AABB result;
    result.min = glm::vec3{std::numeric_limits<float>::max()};
    result.max = glm::vec3{std::numeric_limits<float>::lowest()};

    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 local{(corner & 1) != 0 ? max.x : min.x, (corner & 2) != 0 ? max.y : min.y,
                              (corner & 4) != 0 ? max.z : min.z};
        const glm::vec4 world = matrix * glm::vec4{local, 1.0f};
        const glm::vec3 point = glm::vec3{world} / world.w;
        result.min = glm::min(result.min, point);
        result.max = glm::max(result.max, point);
    }
    return result;
}

} // namespace engine
