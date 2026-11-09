#include "Graphics/ShadowMap.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace engine {

glm::mat4 directionalLightViewProjection(const glm::vec3& lightDirection, const glm::vec3& center,
                                         float extent, float lightDistance, float nearPlane, float farPlane) {
    const glm::vec3 direction = glm::normalize(lightDirection);
    const glm::vec3 lightPosition = center + direction * lightDistance;
    // Any up vector not parallel to the light direction; the key light is
    // steep (y-dominant), so world X is a safe reference.
    const glm::vec3 up = std::abs(direction.x) < 0.9f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::vec3{0.0f, 0.0f, 1.0f};
    const glm::mat4 lightView = glm::lookAt(lightPosition, center, up);
    const glm::mat4 lightProjection = glm::ortho(-extent, extent, -extent, extent, nearPlane, farPlane);
    return lightProjection * lightView;
}

} // namespace engine
