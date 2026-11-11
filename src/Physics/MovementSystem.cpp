#include "Physics/MovementSystem.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/trigonometric.hpp>

namespace engine {
namespace {

// ---------------------------------------------------------------------------
// Source PM_Friction (Quake -> Source lineage), ground plane form:
//
//   speed    = horizontal (ground) speed
//   control  = max(speed, stopSpeed)        // stopspeed floor
//   drop     = control * friction * dt      // sv_friction bleed
//   newSpeed = max(0, speed - drop)
//   velocity *= newSpeed / speed            // direction preserved
//
// The direction is never altered by friction, only the magnitude:
// momentum is a vector, and killing it costs time. Applied on
// EVERY grounded tick, before accelerate and before any jump
// liftoff — a touchdown therefore always pays one friction tick,
// which is what makes held-jump bunny-hopping bleed speed
// instead of preserving it.
// ---------------------------------------------------------------------------
void applyGroundFriction(PlayerState& state, const MoveSettings& settings, float dt) {
    const float speed = std::sqrt(state.velocity.x * state.velocity.x + state.velocity.z * state.velocity.z);
    if (speed <= 0.0f)
        return;
    const float control = std::max(speed, settings.stopSpeed);
    const float drop = control * settings.friction * dt;
    const float newSpeed = std::max(0.0f, speed - drop);
    const float scale = newSpeed / speed;
    state.velocity.x *= scale;
    state.velocity.z *= scale;
}

// ---------------------------------------------------------------------------
// Source PM_Accelerate:
//
//   currentSpeed = dot(velocity, wishDir)
//   addSpeed     = wishSpeed - currentSpeed   (<= 0: no-op)
//   accelSpeed   = min(accelerate * wishSpeed * dt, addSpeed)
//   velocity    += wishDir * accelSpeed
//
// The projection cap is the momentum law: velocity can only grow
// ALONG the wish direction and never past wishSpeed in that
// projection. Reversing direction means currentSpeed is negative,
// so the per-tick budget (accelerate * wishSpeed * dt) must first
// cancel the incoming momentum before any speed builds the other
// way — instant direction flips are mathematically impossible.
// With accelerate == 0 (air control clamp) this adds exactly
// nothing: airborne horizontal velocity is bit-frozen.
// ---------------------------------------------------------------------------
void accelerate(PlayerState& state, const glm::vec3& wishDir, float wishSpeed, float accel, float dt) {
    if (wishSpeed <= 0.0f || accel <= 0.0f)
        return;
    const float currentSpeed = glm::dot(state.velocity, wishDir);
    const float addSpeed = wishSpeed - currentSpeed;
    if (addSpeed <= 0.0f)
        return;
    const float accelSpeed = std::min(accel * wishSpeed * dt, addSpeed);
    state.velocity.x += wishDir.x * accelSpeed;
    state.velocity.z += wishDir.z * accelSpeed;
}

} // namespace

void playerMove(const InputCmd& rawCmd, PlayerState& state, const MoveSettings& settings, float dt) {
    if (dt <= 0.0f)
        return;
    // One legal-input envelope, enforced inside the shared core:
    // neither side can ever simulate an out-of-envelope command.
    const InputCmd cmd = sanitizeInputCmd(rawCmd);

    // Wish direction in world space from facing yaw. Yaw 0 faces
    // -Z (engine forward convention); forward input rides facing.
    const float yaw = glm::radians(cmd.yawDegrees);
    const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
    const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
    glm::vec3 wishDir = forward * cmd.forward + right * cmd.strafe;
    const float wishLength = glm::length(wishDir);
    float wishSpeed = 0.0f;
    if (wishLength > 1e-6f) {
        wishDir /= wishLength;
        wishSpeed = std::min(wishLength, 1.0f) * settings.maxSpeed;
    }

    if (state.grounded) {
        // 1) Friction FIRST — always, even on a jump tick.
        applyGroundFriction(state, settings, dt);
        // 2) Ground accelerate (deliberate ramp-up budget).
        accelerate(state, wishDir, wishSpeed, settings.accelerate, dt);
    } else {
        // Airborne: air accelerate only. The default settings pin
        // this to exactly 0.0 (strict anti-bunny-hop clamp), so a
        // jumping player's horizontal velocity is whatever it was
        // at liftoff — gravity shapes the arc, nothing stacks.
        accelerate(state, wishDir, wishSpeed, settings.airAccelerate, dt);
    }

    // 3) Jump / gravity. Jump neither adds nor removes horizontal
    // velocity; it only exchanges groundedness for vertical speed.
    if (state.grounded && cmd.jump) {
        state.velocity.y = settings.jumpSpeed;
        state.grounded = false;
    }
    if (!state.grounded) {
        state.velocity.y -= settings.gravity * dt;
    } else {
        state.velocity.y = 0.0f;
    }

    // 4) Integrate + ground clamp. Landing restores groundedness;
    // the NEXT tick's friction (step 1) is the landing penalty.
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

glm::vec3 clipVelocity(const glm::vec3& velocity, const glm::vec3& normal, float overbounce) {
    // Source PM_ClipVelocity: subtract the into-plane component,
    // scaled by overbounce, then clean up any residual into-plane
    // error float rounding left behind.
    const float backoff = glm::dot(velocity, normal) * overbounce;
    glm::vec3 out = velocity - normal * backoff;
    const float adjust = glm::dot(out, normal);
    if (adjust < 0.0f) {
        out -= normal * adjust;
    }
    return out;
}

glm::vec3 clipVelocityHorizontal(const glm::vec3& velocity, const glm::vec3& normal, float overbounce) {
    // The wall clip: only the horizontal components participate, so
    // a steep contact can redirect horizontal motion (sliding) but
    // can never manufacture vertical velocity. A purely vertical
    // normal (floor/ceiling) has no horizontal plane to clip against.
    const glm::vec2 planeNormal{normal.x, normal.z};
    const float planeLength = glm::length(planeNormal);
    if (planeLength <= 1e-8f) {
        return velocity;
    }
    const glm::vec2 unit = planeNormal / planeLength;
    const glm::vec2 horizontal{velocity.x, velocity.z};
    const float backoff = glm::dot(horizontal, unit) * overbounce;
    glm::vec2 clipped = horizontal - unit * backoff;
    const float adjust = glm::dot(clipped, unit);
    if (adjust < 0.0f) {
        clipped -= unit * adjust;
    }
    return glm::vec3{clipped.x, velocity.y, clipped.y};
}

bool isWalkableNormal(const glm::vec3& normal, float maxSlopeAngleDegrees) {
    const float clampedDegrees = std::clamp(maxSlopeAngleDegrees, 0.0f, 90.0f);
    return normal.y >= std::cos(glm::radians(clampedDegrees));
}

InputCmd sanitizeInputCmd(const InputCmd& cmd) {
    InputCmd out = cmd; // cmdNumber always preserved
    const auto clampAxis = [](float value) {
        if (!std::isfinite(value))
            return 0.0f;
        return std::clamp(value, -1.0f, 1.0f);
    };
    out.forward = clampAxis(out.forward);
    out.strafe = clampAxis(out.strafe);
    if (!std::isfinite(out.yawDegrees))
        out.yawDegrees = 0.0f;
    out.yawDegrees = std::fmod(out.yawDegrees, 360.0f);
    if (out.yawDegrees < 0.0f)
        out.yawDegrees += 360.0f;
    // Wish vector length <= 1 (diagonal clamp).
    const float length = std::sqrt(out.forward * out.forward + out.strafe * out.strafe);
    if (length > 1.0f) {
        out.forward /= length;
        out.strafe /= length;
    }
    return out;
}

} // namespace engine
