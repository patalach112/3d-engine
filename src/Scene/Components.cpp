#include "Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace engine {

glm::mat4 TransformComponent::modelMatrix() const {
    glm::mat4 model = glm::translate(glm::mat4{1.0f}, position);
    if (useQuaternionOrientation) {
        // Physics path: the exact quaternion the physics engine
        // integrated, converted straight to a matrix — no Euler
        // round-trip, so the rendered orientation is 1:1 with the
        // collider geometry (mat4_cast also makes the q / -q double
        // cover irrelevant: both signs give the same matrix).
        model *= glm::mat4_cast(glm::normalize(orientation));
    } else {
        model = glm::rotate(model, glm::radians(rotationDegrees.y), glm::vec3{0.0f, 1.0f, 0.0f});
        model = glm::rotate(model, glm::radians(rotationDegrees.x), glm::vec3{1.0f, 0.0f, 0.0f});
        model = glm::rotate(model, glm::radians(rotationDegrees.z), glm::vec3{0.0f, 0.0f, 1.0f});
    }
    model = glm::scale(model, scale);
    return model;
}

} // namespace engine
