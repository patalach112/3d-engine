#include "Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace engine {

glm::mat4 TransformComponent::modelMatrix() const {
    glm::mat4 model = glm::translate(glm::mat4{1.0f}, position);
    model = glm::rotate(model, glm::radians(rotationDegrees.y), glm::vec3{0.0f, 1.0f, 0.0f});
    model = glm::rotate(model, glm::radians(rotationDegrees.x), glm::vec3{1.0f, 0.0f, 0.0f});
    model = glm::rotate(model, glm::radians(rotationDegrees.z), glm::vec3{0.0f, 0.0f, 1.0f});
    model = glm::scale(model, scale);
    return model;
}

} // namespace engine
