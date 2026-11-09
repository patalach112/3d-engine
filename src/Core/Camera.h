#pragma once
#include "Core/Export.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
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

    // Exponential smoothing time constants, in seconds. Mouse samples are
    // still measured in pixels (never multiplied by frame time); these
    // values only control how quickly the rendered orientation/velocity
    // converges to the accumulated target. Using 1-exp(-dt/tau) makes the
    // convergence rate identical at 30, 60, 144, ... FPS. 0 disables it.
    float lookSmoothingSeconds = 0.01f;
    float movementSmoothingSeconds = 0.03f;

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
//   camera.update(deltaSeconds);
//   camera.processMovement(input, deltaSeconds);
// ---------------------------------------------------------------------------
class ENGINE_CORE_API Camera {
public:
    explicit Camera(const CameraConfig& config);

    // Accumulates mouse motion (pixels) into the target yaw/pitch,
    // honoring sensitivity, optional Y inversion, pitch clamping and yaw
    // wrapping. Call update() once per frame to converge the rendered
    // orientation towards that target.
    void processLook(glm::vec2 mouseDeltaPixels);

    // Advances look smoothing by the frame delta. The factor is
    // 1-exp(-delta/tau), so the same wall-clock mouse motion settles in
    // the same time regardless of frame rate.
    void update(double deltaSeconds);

    // WASD translation + Q/E vertical movement + Shift boost, integrated
    // with the frame's delta time. Diagonal movement is normalized, so it
    // is never faster than straight movement.
    void processMovement(const Input& input, double deltaSeconds);

    // Integrates one frame of movement input. The direction uses the
    // same (right, worldUp, front) basis as translate(), but velocity is
    // exponentially smoothed before being multiplied by deltaSeconds.
    // Kept separate from processMovement so frame-rate consistency can be
    // unit-tested without a window.
    void updateMovement(glm::vec3 localDirection, double deltaSeconds, bool boost = false);

    // Advances the same exponential movement smoothing used by
    // updateMovement(), but returns the world-space velocity instead of
    // moving the camera. Physics-driven characters use this to drive a
    // rigid body with identical acceleration/deceleration feel.
    [[nodiscard]] glm::vec3 smoothedMovementVelocity(glm::vec3 desiredVelocity, double deltaSeconds);

    [[nodiscard]] float movementSpeed(bool boost = false) const {
        return m_config.movementSpeed * (boost ? m_config.boostMultiplier : 1.0f);
    }

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
    [[nodiscard]] float targetYawDegrees() const { return m_targetYawDegrees; }
    [[nodiscard]] float targetPitchDegrees() const { return m_targetPitchDegrees; }
    [[nodiscard]] const glm::vec3& velocity() const { return m_velocity; }

    void setPosition(const glm::vec3& position) { m_position = position; }
    void setOrientation(float yawDegrees, float pitchDegrees);

private:
    void updateVectors();

    CameraConfig m_config;
    glm::vec3 m_position;
    float m_yawDegrees;
    float m_pitchDegrees;
    float m_targetYawDegrees = 0.0f;
    float m_targetPitchDegrees = 0.0f;
    glm::vec3 m_velocity{0.0f};

    glm::vec3 m_front{0.0f, 0.0f, -1.0f};
    glm::vec3 m_right{1.0f, 0.0f, 0.0f};
    glm::vec3 m_up{0.0f, 1.0f, 0.0f};
};

} // namespace engine
