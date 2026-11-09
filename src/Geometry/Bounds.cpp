#include "Geometry/Bounds.h"

#include <glm/common.hpp>
#include <glm/vec4.hpp>

#include <limits>

namespace engine {

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
