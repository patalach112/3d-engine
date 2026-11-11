#pragma once
#include "Core/Export.h"

#include <glm/vec3.hpp>

namespace engine {

// ---------------------------------------------------------------------------
// Ground friction movement controller (Source-engine style).
//
// One pure function, groundMove(), integrates a player's move state
// from one input command: friction bleed, wish-direction accelerate
// with the classic projected-speed cap, jump, gravity, ground clamp.
// It is deliberately free of engine state so the DEDICATED SERVER
// (authoritative) and the CLIENT (prediction) run the identical
// math on identical inputs and agree bit-for-bit; reconciliation
// only has to clean up timing/quantisation drift, never physics
// divergence. Horizontal velocity is the friction/accelerate domain;
// Y is jump/gravity only.
// ---------------------------------------------------------------------------

struct PlayerMoveInput {
    float forward = 0.0f; // -1..1 (W/S)
    float strafe = 0.0f;  // -1..1 (D/A)
    bool jump = false;
    float yawDegrees = 0.0f; // facing; wishdir derives from it
};

struct PlayerMoveState {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool grounded = true;
};

struct MoveSettings {
    float maxSpeed = 6.0f;      // m/s wish-speed cap
    float friction = 4.0f;      // ground friction coefficient
    float stopSpeed = 1.5f;     // friction control floor (Source stopspeed)
    float accelerate = 10.0f;   // ground acceleration
    float airAccelerate = 2.0f; // air acceleration (weaker, Source-like)
    float gravity = 20.0f;      // m/s^2
    float jumpSpeed = 6.5f;     // m/s initial jump velocity
    float groundY = 0.0f;       // controller ground plane
};

// Advances one step of dt seconds. Deterministic: same state +
// same input + same dt => same result on every machine/build.
ENGINE_NET_API void groundMove(PlayerMoveState& state, const PlayerMoveInput& input, const MoveSettings& settings,
                               float dt);

// Clamps a raw input command into the legal envelope (axes -1..1,
// wish vector length <= 1, finite yaw). The server applies this to
// every received command — clients can ask, never exceed.
[[nodiscard]] ENGINE_NET_API PlayerMoveInput sanitizeMoveInput(const PlayerMoveInput& input);

} // namespace engine
