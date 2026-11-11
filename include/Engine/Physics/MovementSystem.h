#pragma once
#include "Core/Export.h"

#include <cstdint>
#include <glm/vec3.hpp>

namespace engine {

// ---------------------------------------------------------------------------
// MovementSystem: the precise Source-engine movement core.
// Lives in the physics layer (EngineScene, next to PhysicsSystem)
// and is deliberately transport-agnostic: EngineNet merely passes
// InputCmds to it, and the local PlayerSystem ticks it directly.
//
// ONE pure simulation function — playerMove() — is executed by
// every movement consumer with bit-identical results:
//   * the local player controller ticks it in the input loop
//     (singleplayer / standalone / editor Play);
//   * the multiplayer client calls it per local tick for instant
//     prediction;
//   * vibe_server calls it per consumed InputCmd in its 128 Hz
//     authoritative loop.
// Determinism contract: IEEE-754 float math in a fixed order, no
// fast-math, no wall-clock, no randomness. Same InputCmd stream +
// same MoveSettings + same dt => same PlayerState, everywhere.
//
// The settings are the Source "cvars" of the model (sv_friction,
// stopspeed, sv_accelerate, sv_airaccelerate), tuned heavy for
// survival horror and shared verbatim through the Welcome packet,
// so prediction can never drift from the authority by rules.
//
// Rules implemented (see MovementSystem.cpp for the math):
//   1. Ground friction (Source PM_Friction): speed bleed of
//      max(speed, stopSpeed) * friction * dt, applied FIRST on
//      every grounded tick — including jump ticks, so every
//      touchdown pays one friction tick before liftoff. That is
//      the anti-bunny-hop bite.
//   2. Ground accelerate (Source PM_Accelerate): velocity grows
//      only along the wish direction, capped so the projected
//      speed never exceeds maxSpeed. Momentum is a vector:
//      reversing direction must first cancel the existing
//      velocity through the friction+accel budget, never flip.
//   3. Air control is STRICTLY zero by default
//      (airAccelerate = 0): while airborne, horizontal velocity
//      is bit-frozen — no air-strafing, no velocity stacking,
//      no bunny-hop speed retention. A jump only sets vertical
//      velocity; gravity alone shapes the arc.
// ---------------------------------------------------------------------------

struct MoveSettings {
    float maxSpeed = 4.5f;      // wish-speed cap (m/s): deliberate horror pace
    float friction = 8.0f;      // sv_friction: heavy, grounded bleed
    float stopSpeed = 1.0f;     // stopspeed: friction control floor
    float accelerate = 6.0f;    // sv_accelerate (ground): slow ramp-up
    float airAccelerate = 0.0f; // sv_airaccelerate: ZERO — anti-bunny-hop clamp
    float gravity = 20.0f;      // m/s^2
    float jumpSpeed = 6.0f;     // m/s initial vertical velocity
    float groundY = 0.0f;       // ground plane height
};

// One sequenced movement command. cmdNumber is the reconciliation
// key: the server reports the last cmdNumber it consumed inside
// every snapshot, and the client replays every prediction with a
// higher cmdNumber on top of the authoritative state.
struct InputCmd {
    std::uint32_t cmdNumber = 0; // sequence number (1-based per connection)
    float forward = 0.0f;        // -1..1 (W/S)
    float strafe = 0.0f;         // -1..1 (D/A)
    bool jump = false;
    float yawDegrees = 0.0f;     // facing; the wish direction derives from it
};

// The complete movement state the simulation integrates. Plain
// data; TransformComponent mirrors position for rendering/other
// systems, this struct is the movement truth.
struct PlayerState {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool grounded = true;
};

// The shared movement step (Source PM_Move equivalent for the
// ground/air controller): friction -> accelerate -> jump/gravity
// -> integrate. The command is sanitised internally, so the legal
// input envelope is enforced by the same code on both sides.
ENGINE_SCENE_API void playerMove(const InputCmd& cmd, PlayerState& state, const MoveSettings& settings, float dt);

// Clamps a raw command into the legal envelope: axes to -1..1,
// wish vector length <= 1, non-finite values to 0, yaw normalised
// to [0, 360). cmdNumber is always preserved. Clients may ask for
// anything; the simulation only ever honours this envelope.
[[nodiscard]] ENGINE_SCENE_API InputCmd sanitizeInputCmd(const InputCmd& cmd);

// Settings-holding facade so hosts write exactly
// `movement.playerMove(cmd, state, dt)` — the identical call on
// client and server. Delegates to the free function above so
// there is exactly one implementation in the binary.
class MovementSystem {
public:
    explicit MovementSystem(MoveSettings settings = {}) : m_settings(settings) {}

    [[nodiscard]] const MoveSettings& settings() const { return m_settings; }
    void setSettings(const MoveSettings& settings) { m_settings = settings; }

    void playerMove(const InputCmd& cmd, PlayerState& state, float dt) const {
        engine::playerMove(cmd, state, m_settings, dt);
    }

private:
    MoveSettings m_settings;
};

// ---------------------------------------------------------------------------
// CharacterControllerComponent: binds the movement core to a
// physical player entity (Transform + RigidBody + capsule
// Collider, the PlayerSystem player contract). PlayerSystem's
// character path integrates `state` through playerMove() with the
// caller's real dt and writes the resulting velocity into the
// rigid body; PhysicsSystem then integrates the capsule and
// resolves collisions, and the resolved transform is read back
// as the next tick's state.position (the ground/collision
// handshake). Authoring notes:
//   * settings.groundY is the capsule CENTRE height when resting
//     on the floor (0.75 m for the standard 1.50 m capsule), i.e.
//     the ground plane the movement ground-check clamps to;
//   * the controller owns gravity and sleep while driven: the
//     rigid body's useGravity/allowSleep are forced off so
//     PhysicsSystem never double-integrates them.
// Runtime-authored (code/editor), intentionally not serialized:
// networked players carry the same state in NetPlayerComponent.
// ---------------------------------------------------------------------------
struct CharacterControllerComponent {
    MoveSettings settings{}; // friction 8 / accelerate 6 / airAccelerate 0 defaults
    PlayerState state{};     // movement truth between ticks
    std::uint32_t cmdCounter = 0; // local cmd_number sequence
    bool enabled = true;
};

} // namespace engine
