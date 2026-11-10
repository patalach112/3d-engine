#pragma once

// ---------------------------------------------------------------------------
// Newtonian 6-DOF flight model for the SpaceFlightModule (Phase 24).
//
// Pure math, no engine linkage: the game module turns pilot input
// into body-frame forces/torques and lets the engine's PhysicsSystem
// integrate them (F = ma, tau = I alpha, angular momentum conserved
// when no torque acts). There is no atmospheric drag anywhere in this
// model — the only non-input torque is the RCS flight-assist damper,
// which is itself just thrusters doing work, and defaults to a gentle
// stabilization rate.
//
// This header is shared verbatim between the plugin binary and the
// host-side unit tests, so the module's flight behavior is verified
// against exactly the math it ships with.
//
// Ship frame (engine camera convention): -Z forward, +Y up,
// +X starboard. Forces/torques are computed in that frame; the module
// rotates them into world space with the body's orientation.
//
// Editing FlightParams defaults + rebuilding the module + the
// engine's hot-reload poll is the intended tuning workflow: the new
// numbers fly the existing ship, in the existing scene, instantly.
// ---------------------------------------------------------------------------

#include "Scene/Reflection.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace spaceflight {

inline constexpr float kShipMass = 1200.0f; // kg
inline constexpr int kAsteroidCount = 56;

struct FlightParams {
    float mainThrust = 24000.0f;   // N, main engine along -Z (W) / +Z (S)
    float strafeThrust = 9000.0f;  // N, RCS translation (A/D, Space/Ctrl)
    float pitchTorque = 4200.0f;   // N·m about +X
    float yawTorque = 4200.0f;     // N·m about +Y
    float rollTorque = 3000.0f;    // N·m about +Z
    // RCS flight assist: opposes existing spin with torque
    // -angularDamping * I * omega. 0 = pure Newtonian drift.
    float angularDamping = 0.35f; // 1/s
    // Mouse pixels -> dimensionless torque demand.
    float mouseTorqueScale = 0.045f;
};

// All channels in [-1, 1]. main: +1 = W. strafe: +1 = D (starboard).
// lift: +1 = Space (dorsal). roll: +1 = E (starboard-down roll).
// pitch: +1 = nose up. yaw: +1 = nose left (CCW from above).
struct FlightInput {
    float main = 0.0f;
    float strafe = 0.0f;
    float lift = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
};

struct FlightForces {
    glm::vec3 forceLocal{0.0f};  // N, ship frame
    glm::vec3 torqueLocal{0.0f}; // N·m, ship frame
    float mainThrottle = 0.0f;    // |main|, 0..1 (exhaust/light driver)
    float lateralThrottle = 0.0f; // max(|strafe|, |lift|), 0..1
    float totalThrottle = 0.0f;   // blended 0..1 for glow/bloom feedback
};

// angularVelocityLocal: rad/s in ship frame. inertiaLocal: diagonal
// of the body-frame inertia tensor (kg·m²).
[[nodiscard]] inline FlightForces computeFlightForces(const FlightParams& params, const FlightInput& rawInput,
                                                      const glm::vec3& angularVelocityLocal,
                                                      const glm::vec3& inertiaLocal) {
    const FlightInput input{
        std::clamp(rawInput.main, -1.0f, 1.0f),   std::clamp(rawInput.strafe, -1.0f, 1.0f),
        std::clamp(rawInput.lift, -1.0f, 1.0f),   std::clamp(rawInput.pitch, -1.0f, 1.0f),
        std::clamp(rawInput.yaw, -1.0f, 1.0f),    std::clamp(rawInput.roll, -1.0f, 1.0f),
    };

    FlightForces out;
    out.forceLocal = glm::vec3{input.strafe * params.strafeThrust, input.lift * params.strafeThrust,
                               -input.main * params.mainThrust};

    glm::vec3 torque{input.pitch * params.pitchTorque, input.yaw * params.yawTorque, input.roll * params.rollTorque};
    // RCS flight-assist damper: proportional counter-torque. This is
    // the one deliberate deviation from hands-off Newtonian drift,
    // and it is real torque — angular momentum is carried away by
    // the (unmodeled) propellant, exactly like a real RCS loop.
    torque -= params.angularDamping * inertiaLocal * angularVelocityLocal;
    out.torqueLocal = torque;

    out.mainThrottle = std::abs(input.main);
    out.lateralThrottle = std::max(std::abs(input.strafe), std::abs(input.lift));
    const float rotationDemand = std::max({std::abs(input.pitch), std::abs(input.yaw), std::abs(input.roll)});
    out.totalThrottle = std::clamp(out.mainThrottle + 0.6f * out.lateralThrottle + 0.35f * rotationDemand, 0.0f, 1.0f);
    return out;
}

// Editor-facing metadata (Phase 25): the same macro registry the
// engine components use, so an inspector can tune flight parameters
// generically. Registered with ReflectionRegistry by the module.
ENGINE_REFLECT_BEGIN(FlightParams)
    ENGINE_REFLECT_PROPERTY(mainThrust, "Main Thrust (N)")
    ENGINE_REFLECT_PROPERTY(strafeThrust, "Strafe Thrust (N)")
    ENGINE_REFLECT_PROPERTY(pitchTorque, "Pitch Torque (N·m)")
    ENGINE_REFLECT_PROPERTY(yawTorque, "Yaw Torque (N·m)")
    ENGINE_REFLECT_PROPERTY(rollTorque, "Roll Torque (N·m)")
    ENGINE_REFLECT_PROPERTY(angularDamping, "Angular Damping (1/s)")
    ENGINE_REFLECT_PROPERTY(mouseTorqueScale, "Mouse Torque Scale")
ENGINE_REFLECT_END()

} // namespace spaceflight
