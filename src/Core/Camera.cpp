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

// Frame-rate-independent exponential smoothing factor. A fixed per-frame
// lerp (for example always 0.2) converges twice as fast at 120 FPS as at
// 60 FPS; 1-exp(-dt/tau) has the same wall-clock response at every rate.
float smoothingFactor(double deltaSeconds, float timeConstantSeconds) {
    if (deltaSeconds <= 0.0) {
        return 0.0f;
    }
    if (timeConstantSeconds <= 0.0f) {
        return 1.0f;
    }
    return static_cast<float>(1.0 - std::exp(-deltaSeconds / static_cast<double>(timeConstantSeconds)));
}
} // namespace

Camera::Camera(const CameraConfig& config) : m_config(config) {
    reset();
}

void Camera::reset() {
    m_position = m_config.startPosition;
    m_yawDegrees = m_config.startYawDegrees;
    m_pitchDegrees = m_config.startPitchDegrees;
    m_targetYawDegrees = m_yawDegrees;
    m_targetPitchDegrees = m_pitchDegrees;
    m_velocity = glm::vec3{0.0f};
    updateVectors();
}

void Camera::setOrientation(float yawDegrees, float pitchDegrees) {
    m_yawDegrees = wrapDegrees(yawDegrees);
    m_pitchDegrees = std::clamp(pitchDegrees, -m_config.pitchLimitDegrees, m_config.pitchLimitDegrees);
    m_targetYawDegrees = m_yawDegrees;
    m_targetPitchDegrees = m_pitchDegrees;
    m_velocity = glm::vec3{0.0f};
    updateVectors();
}

void Camera::processLook(glm::vec2 mouseDeltaPixels) {
    if (mouseDeltaPixels.x == 0.0f && mouseDeltaPixels.y == 0.0f) {
        return;
    }
    // Sensitivity is degrees per pixel. It must not be multiplied by the
    // frame delta: GLFW already reports the distance moved since the last
    // poll, so doing so would make look speed depend on frame rate.
    const float yawDelta = mouseDeltaPixels.x * m_config.mouseSensitivity;
    // Cursor Y grows downwards: moving the mouse up produces a negative
    // delta and should look up (positive pitch), hence the subtraction.
    const float pixelPitchDelta = mouseDeltaPixels.y * m_config.mouseSensitivity;
    const float pitchDelta = m_config.invertY ? pixelPitchDelta : -pixelPitchDelta;

    m_targetYawDegrees = wrapDegrees(m_targetYawDegrees + yawDelta);
    m_targetPitchDegrees = std::clamp(m_targetPitchDegrees + pitchDelta, -m_config.pitchLimitDegrees,
                                      m_config.pitchLimitDegrees);
    if (m_config.lookSmoothingSeconds <= 0.0f) {
        m_yawDegrees = m_targetYawDegrees;
        m_pitchDegrees = m_targetPitchDegrees;
        updateVectors();
    }
}

void Camera::update(double deltaSeconds) {
    if (deltaSeconds <= 0.0) {
        return;
    }
    const float yawDelta = wrapDegrees(m_targetYawDegrees - m_yawDegrees);
    const float pitchDelta = m_targetPitchDegrees - m_pitchDegrees;
    if (std::abs(yawDelta) < 1e-5f && std::abs(pitchDelta) < 1e-5f) {
        if (m_yawDegrees != m_targetYawDegrees || m_pitchDegrees != m_targetPitchDegrees) {
            m_yawDegrees = m_targetYawDegrees;
            m_pitchDegrees = m_targetPitchDegrees;
            updateVectors();
        }
        return;
    }

    const float factor = smoothingFactor(deltaSeconds, m_config.lookSmoothingSeconds);
    if (factor >= 0.999999f) {
        m_yawDegrees = m_targetYawDegrees;
        m_pitchDegrees = m_targetPitchDegrees;
    } else {
        m_yawDegrees = wrapDegrees(m_yawDegrees + yawDelta * factor);
        m_pitchDegrees = std::clamp(m_pitchDegrees + pitchDelta * factor, -m_config.pitchLimitDegrees,
                                    m_config.pitchLimitDegrees);
        // Snap the final sub-thousandth-degree tail instead of spending
        // dozens of high-refresh frames asymptotically approaching it.
        if (std::abs(wrapDegrees(m_targetYawDegrees - m_yawDegrees)) < 1e-4f &&
            std::abs(m_targetPitchDegrees - m_pitchDegrees) < 1e-4f) {
            m_yawDegrees = m_targetYawDegrees;
            m_pitchDegrees = m_targetPitchDegrees;
        }
    }
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
    updateMovement(localDirection, deltaSeconds, boost);
}

void Camera::updateMovement(glm::vec3 localDirection, double deltaSeconds, bool boost) {
    if (deltaSeconds <= 0.0) {
        return;
    }

    glm::vec3 desiredVelocity{0.0f};
    if (glm::dot(localDirection, localDirection) != 0.0f) {
        const glm::vec3 direction = glm::normalize(localDirection);
        const float speed = m_config.movementSpeed * (boost ? m_config.boostMultiplier : 1.0f);
        desiredVelocity = (m_right * direction.x + kWorldUp * direction.y + m_front * direction.z) * speed;
    }

    (void)smoothedMovementVelocity(desiredVelocity, deltaSeconds);

    // Velocity is in world units per second; multiplying by this frame's
    // real elapsed time gives frame-rate-independent travel distance.
    m_position += m_velocity * static_cast<float>(deltaSeconds);
}

glm::vec3 Camera::smoothedMovementVelocity(glm::vec3 desiredVelocity, double deltaSeconds) {
    if (deltaSeconds <= 0.0) {
        return m_velocity;
    }
    if (m_config.movementSmoothingSeconds <= 0.0f) {
        m_velocity = desiredVelocity;
    } else {
        const float factor = smoothingFactor(deltaSeconds, m_config.movementSmoothingSeconds);
        m_velocity += (desiredVelocity - m_velocity) * factor;
        if (glm::dot(desiredVelocity, desiredVelocity) == 0.0f &&
            glm::dot(m_velocity, m_velocity) < 1e-10f) {
            m_velocity = glm::vec3{0.0f};
        }
    }
    return m_velocity;
}

void Camera::translate(glm::vec3 localDirection, double deltaSeconds, bool boost) {
    if (glm::dot(localDirection, localDirection) == 0.0f || deltaSeconds <= 0.0) {
        return;
    }
    const glm::vec3 direction = glm::normalize(localDirection);
    const float speed = m_config.movementSpeed * (boost ? m_config.boostMultiplier : 1.0f);
    const float distance = speed * static_cast<float>(deltaSeconds);
    m_position += (m_right * direction.x + kWorldUp * direction.y + m_front * direction.z) * distance;
    // This is an instantaneous displacement API; do not let an older
    // smoothed velocity unexpectedly continue it on the next frame.
    m_velocity = glm::vec3{0.0f};
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
