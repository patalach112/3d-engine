#include "Net/FrictionController.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

namespace engine {

void groundMove(PlayerMoveState& state, const PlayerMoveInput& input, const MoveSettings& settings, float dt) {
    if (dt <= 0.0f)
        return;

    // Wish direction in world space from facing yaw. Yaw 0 faces -Z
    // (engine forward convention); forward input rides the facing.
    const float yaw = glm::radians(input.yawDegrees);
    const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
    const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
    glm::vec3 wishDir = forward * input.forward + right * input.strafe;
    const float wishLength = glm::length(wishDir);
    float wishSpeed = 0.0f;
    if (wishLength > 1e-6f) {
        wishDir /= wishLength;
        wishSpeed = std::min(wishLength, 1.0f) * settings.maxSpeed;
    }

    if (state.grounded) {
        // --- Ground friction (Source Friction()): bleed speed by
        // max(speed, stopSpeed) * friction * dt, never below zero.
        const float speed = glm::length(state.velocity);
        if (speed > 0.0f) {
            const float control = std::max(speed, settings.stopSpeed);
            const float drop = control * settings.friction * dt;
            const float newSpeed = std::max(0.0f, speed - drop);
            const float scale = newSpeed / speed;
            state.velocity.x *= scale;
            state.velocity.z *= scale;
        }
    }

    // --- Accelerate (Source Accelerate()): project current velocity
    // on the wish direction; add at most (wishSpeed - projection)
    // and at most accel * wishSpeed * dt.
    if (wishSpeed > 0.0f) {
        const float currentSpeed = glm::dot(state.velocity, wishDir);
        const float addSpeed = wishSpeed - currentSpeed;
        if (addSpeed > 0.0f) {
            const float accelSpeed =
                std::min(settings.accelerate * wishSpeed * dt, addSpeed);
            const float applied = state.grounded ? accelSpeed
                                                 : std::min(settings.airAccelerate * wishSpeed * dt, addSpeed);
            state.velocity.x += wishDir.x * applied;
            state.velocity.z += wishDir.z * applied;
        }
    }

    // --- Jump / gravity.
    if (state.grounded && input.jump) {
        state.velocity.y = settings.jumpSpeed;
        state.grounded = false;
    }
    if (!state.grounded) {
        state.velocity.y -= settings.gravity * dt;
    } else {
        state.velocity.y = 0.0f;
    }

    // --- Integrate + ground clamp.
    state.position += state.velocity * dt;
    if (state.position.y <= settings.groundY) {
        state.position.y = settings.groundY;
        if (state.velocity.y < 0.0f)
            state.velocity.y = 0.0f;
        state.grounded = true;
    } else {
        state.grounded = false;
    }
}

PlayerMoveInput sanitizeMoveInput(const PlayerMoveInput& input) {
    PlayerMoveInput out = input;
    auto clampAxis = [](float value) {
        if (!std::isfinite(value))
            return 0.0f;
        return std::clamp(value, -1.0f, 1.0f);
    };
    out.forward = clampAxis(out.forward);
    out.strafe = clampAxis(out.strafe);
    if (!std::isfinite(out.yawDegrees))
        out.yawDegrees = 0.0f;
    // Normalise yaw into [0, 360).
    out.yawDegrees = std::fmod(out.yawDegrees, 360.0f);
    if (out.yawDegrees < 0.0f)
        out.yawDegrees += 360.0f;
    // Wish vector length <= 1 (diagonal clamp, Source PM_ClipVelocity-style).
    const float length = std::sqrt(out.forward * out.forward + out.strafe * out.strafe);
    if (length > 1.0f) {
        out.forward /= length;
        out.strafe /= length;
    }
    return out;
}

} // namespace engine
