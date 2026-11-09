#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace engine {

class Input;

// Camera tuning (see [Camera] in config/engine.ini). Angles are degrees.
struct CameraConfig {
    glm::vec3 startPosition{0.0f, 0.0f, 3.0f};
    float startYawDegrees = -90.0f; // -90 looks down -Z, towards the origin
    float startPitchDegrees = 0.0f;

    float movementSpeed = 3.0f;    // world units per second
    float boostMultiplier = 2.5f;  // applied while Shift is held
    float mouseSensitivity = 0.1f; // degrees per pixel of mouse movement
    bool invertY = false;

    // Absolute pitch limit. Staying below 90 degrees keeps the camera's
    // front vector from ever aligning with the world up vector, which is
    // what prevents gimbal lock (and the view flipping upside down).
    float pitchLimitDegrees = 89.0f;

    float fieldOfViewDegrees = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

// ---------------------------------------------------------------------------
// Camera — first-person (fly) camera.
//
// Orientation is stored as yaw/pitch Euler angles and converted to an
// orthonormal basis (front/right/up) on every change; the view matrix is
// glm::lookAt over that basis. Movement follows the camera's orientation
// (W flies towards the crosshair, including pitch), with Q/E for
// world-vertical descent/ascent.
//
// Typical per-frame use from the variable update:
//   camera.processLook(input.mouseDelta());
//   camera.processMovement(input, deltaSeconds);
// ---------------------------------------------------------------------------
class Camera {
public:
    explicit Camera(const CameraConfig& config);

    // Applies mouse motion (pixels) to yaw/pitch, honoring sensitivity,
    // optional Y inversion, pitch clamping and yaw wrapping.
    void processLook(glm::vec2 mouseDeltaPixels);

    // WASD translation + Q/E vertical movement + Shift boost, integrated
    // with the frame's delta time. Diagonal movement is normalized, so it
    // is never faster than straight movement.
    void processMovement(const Input& input, double deltaSeconds);

    // Pure translation along the camera basis: the direction's components
    // are (right, worldUp, front) amounts, scaled by speed * deltaSeconds.
    // Kept separate from processMovement so the math is unit-testable
    // without a window.
    void translate(glm::vec3 localDirection, double deltaSeconds, bool boost = false);

    // Restores the configured start position and orientation.
    void reset();

    [[nodiscard]] glm::mat4 viewMatrix() const;
    // Vulkan projection for the given aspect ratio (width / height),
    // including the clip-space Y flip Vulkan requires.
    [[nodiscard]] glm::mat4 projectionMatrix(float aspectRatio) const;

    [[nodiscard]] const glm::vec3& position() const { return m_position; }
    [[nodiscard]] const glm::vec3& front() const { return m_front; }
    [[nodiscard]] const glm::vec3& right() const { return m_right; }
    [[nodiscard]] const glm::vec3& up() const { return m_up; }
    [[nodiscard]] float yawDegrees() const { return m_yawDegrees; }
    [[nodiscard]] float pitchDegrees() const { return m_pitchDegrees; }

    void setPosition(const glm::vec3& position) { m_position = position; }
    void setOrientation(float yawDegrees, float pitchDegrees);

private:
    void updateVectors();

    CameraConfig m_config;
    glm::vec3 m_position;
    float m_yawDegrees;
    float m_pitchDegrees;

    glm::vec3 m_front{0.0f, 0.0f, -1.0f};
    glm::vec3 m_right{1.0f, 0.0f, 0.0f};
    glm::vec3 m_up{0.0f, 1.0f, 0.0f};
};

} // namespace engine
