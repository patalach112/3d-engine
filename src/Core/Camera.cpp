#include "Core/Camera.h"

#include "Platform/Input.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace engine {
namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

// Wraps an angle to [-180, 180) so yaw never grows without bound.
float wrapDegrees(float degrees) {
    float wrapped = std::fmod(degrees + 180.0f, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    return wrapped - 180.0f;
}
} // namespace

Camera::Camera(const CameraConfig& config) : m_config(config) {
    reset();
}

void Camera::reset() {
    m_position = m_config.startPosition;
    m_yawDegrees = m_config.startYawDegrees;
    m_pitchDegrees = m_config.startPitchDegrees;
    updateVectors();
}

void Camera::setOrientation(float yawDegrees, float pitchDegrees) {
    m_yawDegrees = wrapDegrees(yawDegrees);
    m_pitchDegrees = std::clamp(pitchDegrees, -m_config.pitchLimitDegrees, m_config.pitchLimitDegrees);
    updateVectors();
}

void Camera::processLook(glm::vec2 mouseDeltaPixels) {
    if (mouseDeltaPixels.x == 0.0f && mouseDeltaPixels.y == 0.0f) {
        return;
    }
    m_yawDegrees = wrapDegrees(m_yawDegrees + mouseDeltaPixels.x * m_config.mouseSensitivity);
    // Cursor Y grows downwards: moving the mouse up produces a negative
    // delta and should look up (positive pitch), hence the subtraction.
    const float pitchDelta = mouseDeltaPixels.y * m_config.mouseSensitivity;
    m_pitchDegrees += (m_config.invertY ? pitchDelta : -pitchDelta);
    m_pitchDegrees = std::clamp(m_pitchDegrees, -m_config.pitchLimitDegrees, m_config.pitchLimitDegrees);
    updateVectors();
}

void Camera::processMovement(const Input& input, double deltaSeconds) {
    glm::vec3 localDirection{0.0f};
    if (input.isKeyDown(Key::W)) {
        localDirection.z += 1.0f;
    }
    if (input.isKeyDown(Key::S)) {
        localDirection.z -= 1.0f;
    }
    if (input.isKeyDown(Key::D)) {
        localDirection.x += 1.0f;
    }
    if (input.isKeyDown(Key::A)) {
        localDirection.x -= 1.0f;
    }
    if (input.isKeyDown(Key::E)) {
        localDirection.y += 1.0f;
    }
    if (input.isKeyDown(Key::Q)) {
        localDirection.y -= 1.0f;
    }

    const bool boost = input.isKeyDown(Key::LeftShift) || input.isKeyDown(Key::RightShift);
    translate(localDirection, deltaSeconds, boost);
}

void Camera::translate(glm::vec3 localDirection, double deltaSeconds, bool boost) {
    if (glm::dot(localDirection, localDirection) == 0.0f || deltaSeconds <= 0.0) {
        return;
    }
    const glm::vec3 direction = glm::normalize(localDirection);
    const float speed = m_config.movementSpeed * (boost ? m_config.boostMultiplier : 1.0f);
    const float distance = speed * static_cast<float>(deltaSeconds);
    m_position += (m_right * direction.x + kWorldUp * direction.y + m_front * direction.z) * distance;
}

glm::mat4 Camera::viewMatrix() const {
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

glm::mat4 Camera::projectionMatrix(float aspectRatio) const {
    glm::mat4 projection =
        glm::perspective(glm::radians(m_config.fieldOfViewDegrees), aspectRatio, m_config.nearPlane,
                         m_config.farPlane);
    // GLM was designed for OpenGL, whose clip-space Y axis points down
    // compared to Vulkan. Flip it, otherwise the image is upside down
    // (the pipeline's clockwise front-face setting compensates winding).
    projection[1][1] *= -1.0f;
    return projection;
}

void Camera::updateVectors() {
    const float yaw = glm::radians(m_yawDegrees);
    const float pitch = glm::radians(m_pitchDegrees);

    glm::vec3 front;
    front.x = std::cos(yaw) * std::cos(pitch);
    front.y = std::sin(pitch);
    front.z = std::sin(yaw) * std::cos(pitch);
    m_front = glm::normalize(front);

    // Pitch is clamped away from +/-90 degrees, so front is never parallel
    // to the world up vector and these cross products stay well-defined.
    m_right = glm::normalize(glm::cross(m_front, kWorldUp));
    m_up = glm::normalize(glm::cross(m_right, m_front));
}

} // namespace engine
