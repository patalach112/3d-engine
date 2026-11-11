#pragma once
#include "Core/Export.h"

#include <cstdint>
#include <span>
#include <glm/mat3x3.hpp>
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
    // Source-style sprint (sv_sprintspeed + aux-power stamina):
    // while the sprint input is held and the player is moving, the
    // wish-speed cap rises to sprintSpeed and stamina drains;
    // exhaustion locks sprint out until stamina recovers past the
    // resume threshold (25%), so sprint can never be feathered.
    float sprintSpeed = 6.8f;          // m/s wish cap while sprinting
    float staminaDrainPerSecond = 0.16f; // ~6 s of sprint from full
    float staminaRegenPerSecond = 0.12f; // full recharge in ~8 s
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
    bool sprint = false;         // sprint key held (Shift)
    float yawDegrees = 0.0f;     // facing; the wish direction derives from it
};

// The complete movement state the simulation integrates. Plain
// data; TransformComponent mirrors position for rendering/other
// systems, this struct is the movement truth.
struct PlayerState {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool grounded = true;
    // Sprint stamina (1 = full). Drained by playerMove while a
    // sprint is effective, regenerated otherwise; sprintExhausted
    // is the lockout latch (set at 0, cleared at 25% charge).
    float stamina = 1.0f;
    bool sprintExhausted = false;
};

// The shared movement step (Source PM_Move equivalent for the
// ground/air controller): friction -> accelerate -> jump/gravity
// -> integrate. The command is sanitised internally, so the legal
// input envelope is enforced by the same code on both sides.
ENGINE_SCENE_API void playerMove(const InputCmd& cmd, PlayerState& state, const MoveSettings& settings, float dt);

// ---------------------------------------------------------------------------
// Velocity clipping against collision planes (Source PM_ClipVelocity).
// clipVelocity removes the into-plane component of a velocity:
//   backoff = dot(velocity, normal) * overbounce
//   result  = velocity - normal * backoff
// (overbounce 1 slides exactly along the plane; Source uses 1 for
// characters, >1 only for bounce pads). A numerical guard then
// removes any residual into-plane error. clipVelocityHorizontal
// performs the same clip purely in the horizontal plane (the normal
// is projected onto XZ first): for contacts steeper than the
// climbable slope the character must NEVER gain or lose height from
// the clip — wall pressure becomes wall sliding, never lift. That
// is what stops a physics solver from resolving horizontal wall
// penetration by displacing the character vertically.
// ---------------------------------------------------------------------------
[[nodiscard]] ENGINE_SCENE_API glm::vec3 clipVelocity(const glm::vec3& velocity, const glm::vec3& normal,
                                                      float overbounce = 1.0f);
[[nodiscard]] ENGINE_SCENE_API glm::vec3 clipVelocityHorizontal(const glm::vec3& velocity, const glm::vec3& normal,
                                                                float overbounce = 1.0f);

// ---------------------------------------------------------------------------
// Capsule-trace CCD (sweep) primitives: the collision half of the
// character controller, kept as pure math so hosts and tests can
// drive it without a scene. The capsule is always upright (its
// segment runs along world Y through the centre). sweepCapsule
// advances the capsule centre along `displacement` and reports
// the EARLIEST impact against a set of world-space boxes: the
// fraction of the displacement actually travelled, the surface
// normal and point at impact. Movement stops exactly at the
// impact point (t_hit) and the caller clips the velocity
// (PM_ClipVelocity) and re-sweeps the remainder — the Source
// bump-and-slide loop — so a capsule of any speed can neither
// tunnel through nor be pushed out of geometry: penetration
// never occurs, therefore no solver feedback can develop.
// Conservative advancement over the exact capsule/box distance
// keeps the trace honest to ~1 mm. A capsule already penetrating
// a box reports startPenetrated with the shallowest exit (normal
// + depth) so the caller can depenetrate ONCE, bounded — never
// through an accumulating solver.
// ---------------------------------------------------------------------------
struct TraceBox {
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{0.5f};
    glm::mat3 axes{1.0f}; // box local axes in world space (columns)
};

struct SweepHit {
    float fraction = 1.0f;              // displacement fraction travelled before impact
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; // impact surface normal (world)
    glm::vec3 point{0.0f};              // impact point on the box surface (world)
    int32_t boxIndex = -1;              // index into the swept box span
    bool startPenetrated = false;       // capsule already overlapped a box at t=0
    float penetrationDepth = 0.0f;      // shallowest-exit depth when startPenetrated
    glm::vec3 penetrationNormal{0.0f, 1.0f, 0.0f}; // shallowest-exit direction
};

// Signed clearance from a capsule (segment segmentA..segmentB
// swept by radius) to an oriented box: positive when separated,
// 0 when the capsule surface touches, negative (down to -radius)
// when the capsule surface overlaps the box. Optionally reports
// the closest points on the capsule segment and on the box.
[[nodiscard]] ENGINE_SCENE_API float capsuleBoxDistance(const glm::vec3& segmentA, const glm::vec3& segmentB,
                                                        float radius, const glm::vec3& boxCenter,
                                                        const glm::mat3& boxAxes, const glm::vec3& boxHalfExtents,
                                                        glm::vec3* outSegmentPoint = nullptr,
                                                        glm::vec3* outBoxPoint = nullptr);

// Sweeps an upright capsule (centre startCenter, segment
// half-length halfHeight, radius) along displacement against
// boxes. Returns true and fills outHit on the earliest impact
// with fraction < 1 (a resting/tangent contact the velocity does
// not approach is NOT an impact: sliding along a floor at exact
// contact distance must not stop the sweep; "approach" is tested
// with a 1e-6 m deadband because closest-point normals carry
// float tilt of that order). Penetrating starts return true
// immediately with fraction 0 + startPenetrated.
[[nodiscard]] ENGINE_SCENE_API bool sweepCapsule(const glm::vec3& startCenter, const glm::vec3& displacement,
                                                 float radius, float halfHeight, std::span<const TraceBox> boxes,
                                                 SweepHit& outHit);

// Walkable-ground test for a surface normal: true when the slope is
// at most maxSlopeAngleDegrees from vertical (45 deg => normal.y
// >= 0.7071). Anything steeper is a wall, never ground: characters
// on it are airborne, get full gravity, and never snap to it.
[[nodiscard]] ENGINE_SCENE_API bool isWalkableNormal(const glm::vec3& normal, float maxSlopeAngleDegrees);

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
//   * the controller owns gravity and sleep while driven: the
//     rigid body's useGravity/allowSleep are forced off so
//     PhysicsSystem never double-integrates them.
// Groundedness is NEVER inferred from contacts or penetration
// depth: every tick casts a vertical shapecast (a small ray fan
// under the capsule's bottom sphere, skinWidth of slack); only a
// walkable normal (within maxSlopeAngleDegrees of up) grounds the
// character, and the capsule then snaps exactly onto the probed
// surface — no floating offset, no vertical step-snapping while
// airborne. A miss or a steep normal means AIRBORNE with full
// gravity: steep slopes can be slid on, never stood on.
// Wall contact is neutralised BEFORE the physics step by clipping
// the velocity horizontally against probed wall planes
// (clipVelocityHorizontal), so the solver never has horizontal
// penetration to convert into upward displacement; the only way
// up is the explicit step-up, offered solely for grounded
// horizontal movement into obstacles at most maxStepHeight tall.
// Runtime-authored (code/editor), intentionally not serialized:
// networked players carry the same state in NetPlayerComponent.
// ---------------------------------------------------------------------------
struct CharacterControllerComponent {
    MoveSettings settings{}; // friction 8 / accelerate 6 / airAccelerate 0 defaults
    PlayerState state{};     // movement truth between ticks
    // Collision tuning (physics layer only; never on the wire).
    float skinWidth = 0.05f;            // ground-probe margin below the capsule (m)
    float maxStepHeight = 0.30f;        // tallest ledge the step-up may claim (m)
    float maxSlopeAngleDegrees = 45.0f; // steepest walkable ground normal
    // Last ground probe result (rewritten every driven tick; state
    // for tests/tools — groundedness itself lives in state.grounded).
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
    float groundY = 0.0f; // world Y of the last probed surface point
    // Fixed-step render interpolation: the capsule centre at the
    // START of the most recent driven tick (the end of the one
    // before it). Hosts blend interpolationFrom -> live transform
    // with the loop's fractional alpha, so rendering at any rate
    // (incl. 1000 fps uncapped) glides between fixed steps instead
    // of juddering at the step rate. See syncCameraInterpolated.
    glm::vec3 interpolationFrom{0.0f};
    bool interpolationValid = false;
    std::uint32_t cmdCounter = 0; // local cmd_number sequence
    bool enabled = true;
};

} // namespace engine
