#include "Scene/Systems/PlayerSystem.h"

#include "Platform/Input.h"
#include "Scene/Raycast.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

namespace engine {
namespace {

struct PlayerParts {
    CameraComponent* camera = nullptr;
    TransformComponent* transform = nullptr;
    RigidBodyComponent* body = nullptr;
    ColliderComponent* collider = nullptr;
};

PlayerParts findPlayer(Scene& scene) {
    PlayerParts parts;
    const entt::entity entity = scene.findActiveCameraEntity();
    if (entity == entt::null) {
        return parts;
    }
    auto& registry = scene.registry();
    if (!registry.all_of<CameraComponent, TransformComponent, RigidBodyComponent, ColliderComponent>(entity)) {
        return parts;
    }
    parts.camera = &registry.get<CameraComponent>(entity);
    parts.transform = &registry.get<TransformComponent>(entity);
    parts.body = &registry.get<RigidBodyComponent>(entity);
    parts.collider = &registry.get<ColliderComponent>(entity);
    return parts;
}

// The character-driven player: the physical player set plus a
// CharacterControllerComponent binding it to the movement core.
struct CharacterParts {
    PlayerParts player;
    CharacterControllerComponent* controller = nullptr;
};

CharacterParts findCharacterPlayer(Scene& scene) {
    CharacterParts parts;
    parts.player = findPlayer(scene);
    if (parts.player.camera == nullptr) {
        return parts;
    }
    const entt::entity entity = scene.findActiveCameraEntity();
    auto& registry = scene.registry();
    if (registry.all_of<CharacterControllerComponent>(entity)) {
        parts.controller = &registry.get<CharacterControllerComponent>(entity);
    }
    return parts;
}


// Raycast for character probes: the closest hit that is neither
// the character itself nor a trigger volume (triggers never ground
// or block a character). raycastAll arrives sorted by distance, so
// the first surviving hit is the closest usable surface.
bool castFiltered(const Scene& scene, const Ray& ray, float maxDistance, uint32_t layerMask, entt::entity self,
                  RaycastHit& outHit) {
    std::vector<RaycastHit> hits;
    (void)scene.raycastAll(ray, hits, maxDistance, layerMask);
    const auto& registry = scene.registry();
    for (const RaycastHit& hit : hits) {
        if (hit.entity == self) {
            continue;
        }
        const ColliderComponent* collider = registry.try_get<ColliderComponent>(hit.entity);
        if (collider != nullptr && collider->isTrigger) {
            continue;
        }
        outHit = hit;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The character ground probe: a downward capsule shapecast,
// approximated by a fan of vertical rays — one under the capsule
// centre and four under the rim of the bottom sphere (0.9 * radius
// out). A capsule standing on a ledge edge is supported by its rim,
// not its centre, so a single centre ray would call it airborne
// the moment it steps up; the fan asks the question the capsule
// actually poses: is there walkable surface within the skin window
// under ANY part of the bottom sphere? Every ray starts half a
// skin above the capsule bottom so a resting (slightly embedded)
// capsule still reports the true surface point and normal rather
// than an inside hit (inside hits are skipped entirely: their
// by-convention up normal once let steep ramps pose as floors).
// The centre ray is tried first, so flat
// ground costs one cast. `point`/`normal` record the first surface
// found (diagnostics); `groundPoint`/`groundNormal` the first
// WALKABLE one (the groundedness verdict).
// ---------------------------------------------------------------------------
struct GroundProbeResult {
    bool hit = false;
    bool walkable = false;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec3 groundPoint{0.0f};
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
};

GroundProbeResult probeGroundFan(const Scene& scene, const glm::vec3& capsuleCenter, float bottomOffset,
                                 float capsuleRadius, float skinWidth, float maxSlopeAngleDegrees,
                                 uint32_t layerMask, entt::entity self) {
    GroundProbeResult result;
    const float lift = skinWidth * 0.5f;
    const float maxDistance = skinWidth + lift;
    const float rim = capsuleRadius * 0.9f;
    const glm::vec3 offsets[5] = {glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{rim, 0.0f, 0.0f},
                                  glm::vec3{-rim, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, rim},
                                  glm::vec3{0.0f, 0.0f, -rim}};
    for (const glm::vec3& offset : offsets) {
        RaycastHit hit;
        const glm::vec3 origin{capsuleCenter.x + offset.x, capsuleCenter.y - bottomOffset + lift,
                               capsuleCenter.z + offset.z};
        if (!castFiltered(scene, Ray{origin, glm::vec3{0.0f, -1.0f, 0.0f}}, maxDistance, layerMask, self, hit)) {
            continue;
        }
        if (hit.distance <= 1e-4f) {
            // Born inside the volume (steep ground rising under a
            // rim ray, or a deeply sunk capsule): an inside hit
            // reports the ray origin and an up normal by convention
            // — neither is a surface. It must never ground anyone.
            continue;
        }
        if (!result.hit) {
            result.hit = true;
            result.point = hit.position;
            result.normal = hit.normal;
        }
        if (isWalkableNormal(hit.normal, maxSlopeAngleDegrees)) {
            result.walkable = true;
            result.groundPoint = hit.position;
            result.groundNormal = hit.normal;
            break;
        }
    }
    return result;
}

} // namespace

bool PlayerSystem::hasPhysicalPlayer(const Scene& scene) {
    const entt::entity entity = scene.findActiveCameraEntity();
    return entity != entt::null &&
           scene.registry().all_of<TransformComponent, RigidBodyComponent, ColliderComponent>(entity);
}

bool PlayerSystem::driveMovement(Scene& scene, glm::vec3 localDirection, double fixedDeltaSeconds, bool boost) {
    const PlayerParts player = findPlayer(scene);
    if (player.camera == nullptr || player.body == nullptr || fixedDeltaSeconds <= 0.0) {
        return false;
    }

    Camera& camera = player.camera->camera;
    glm::vec3 forward = camera.front();
    forward.y = 0.0f;
    forward = glm::dot(forward, forward) > 1e-8f ? glm::normalize(forward) : glm::vec3{0.0f, 0.0f, -1.0f};
    glm::vec3 right = camera.right();
    right.y = 0.0f;
    right = glm::dot(right, right) > 1e-8f ? glm::normalize(right) : glm::vec3{1.0f, 0.0f, 0.0f};

    glm::vec3 wish = right * localDirection.x + forward * localDirection.z;
    if (glm::dot(wish, wish) > 1.0f) {
        wish = glm::normalize(wish);
    }
    const glm::vec3 desired = wish * camera.movementSpeed(boost);
    const glm::vec3 smoothed = camera.smoothedMovementVelocity(glm::vec3{desired.x, 0.0f, desired.z},
                                                               fixedDeltaSeconds);
    player.body->velocity.x = smoothed.x;
    player.body->velocity.z = smoothed.z;
    if (localDirection.y != 0.0f) {
        // Vertical thrust (jump/fly keys) overrides gravity for this step;
        // with no vertical input the physics step's gravity owns Y.
        player.body->velocity.y = localDirection.y * camera.movementSpeed(boost);
    }
    return true;
}

bool PlayerSystem::update(Scene& scene, const Input& input, double fixedDeltaSeconds) {
    // Character-driven players tick the decoupled MovementSystem;
    // everyone else keeps the legacy smoothed-velocity path.
    if (findCharacterPlayer(scene).controller != nullptr) {
        return updateCharacter(scene, input, fixedDeltaSeconds);
    }
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
    if (input.isKeyDown(Key::Space) || input.isKeyDown(Key::E)) {
        localDirection.y += 1.0f;
    }
    if (input.isKeyDown(Key::Q)) {
        localDirection.y -= 1.0f;
    }
    const bool boost = input.isKeyDown(Key::LeftShift) || input.isKeyDown(Key::RightShift);
    return driveMovement(scene, localDirection, fixedDeltaSeconds, boost);
}

bool PlayerSystem::ensureCharacterController(Scene& scene, float groundY) {
    const PlayerParts player = findPlayer(scene);
    if (player.camera == nullptr || player.transform == nullptr) {
        return false;
    }
    const entt::entity entity = scene.findActiveCameraEntity();
    auto& registry = scene.registry();
    if (registry.all_of<CharacterControllerComponent>(entity)) {
        return true;
    }
    CharacterControllerComponent controller;
    controller.settings.groundY = groundY;
    controller.state.position = player.transform->position;
    controller.state.grounded = true;
    registry.emplace<CharacterControllerComponent>(entity, controller);
    return true;
}

bool PlayerSystem::probeGround(const Scene& scene, const glm::vec3& capsuleCenter, float capsuleHalfHeight,
                               float capsuleRadius, float skinWidth, float maxSlopeAngleDegrees, uint32_t layerMask,
                               glm::vec3& outPoint, glm::vec3& outNormal) {
    const GroundProbeResult probe =
        probeGroundFan(scene, capsuleCenter, capsuleHalfHeight + capsuleRadius, capsuleRadius, skinWidth,
                       maxSlopeAngleDegrees, layerMask, scene.findActiveCameraEntity());
    if (!probe.walkable) {
        return false; // miss, or only steep (wall) normals below
    }
    outPoint = probe.groundPoint;
    outNormal = probe.groundNormal;
    return true;
}

bool PlayerSystem::driveCharacter(Scene& scene, const InputCmd& cmd, double deltaSeconds) {
    if (deltaSeconds <= 0.0) {
        return false;
    }
    const CharacterParts parts = findCharacterPlayer(scene);
    if (parts.controller == nullptr || parts.player.body == nullptr || parts.player.transform == nullptr ||
        parts.player.collider == nullptr || !parts.controller->enabled) {
        return false;
    }
    CharacterControllerComponent& controller = *parts.controller;
    RigidBodyComponent& body = *parts.player.body;
    TransformComponent& transform = *parts.player.transform;
    const ColliderComponent& collider = *parts.player.collider;
    const float dt = static_cast<float>(deltaSeconds);

    // World-space capsule dimensions, mirroring PhysicsSystem's
    // geometry build (radius scaled by XZ, segment by Y).
    const glm::vec3 scale = transform.scale;
    const float capsuleRadius = collider.radius * std::max(scale.x, scale.z);
    const float segmentHalf = collider.capsuleHalfHeight * scale.y;
    const float bottomOffset = segmentHalf + capsuleRadius; // centre -> capsule bottom
    const uint32_t layerMask = collider.mask;
    const entt::entity self = scene.findActiveCameraEntity();

    // Handshake, part 1: PhysicsSystem resolved the capsule last
    // step — its transform is the positional truth this tick
    // resumes from. Groundedness is NOT read from it (below).
    controller.state.position = transform.position;

    // ------------------------------------------------------------------
    // Ground probe: vertical shapecast (ray fan) from the capsule
    // bottom, skinWidth of slack. This verdict is the ONLY source of
    // groundedness — contacts, support manifolds and penetration
    // depth are never consulted, so resting against geometry can no
    // longer masquerade as standing on it. A miss or a steep normal
    // (n.y below the slope limit) means AIRBORNE: full gravity, no
    // snapping. A walkable hit while not moving up grounds the
    // character and snaps the capsule exactly onto the surface
    // across the skin gap — landings end ON the floor, never
    // hovering above it (the floating-offset fix).
    // ------------------------------------------------------------------
    glm::vec3 groundPoint{0.0f};
    {
        const glm::vec3 bottom = controller.state.position - glm::vec3{0.0f, bottomOffset, 0.0f};
        const GroundProbeResult probe = probeGroundFan(scene, controller.state.position, bottomOffset, capsuleRadius,
                                                       controller.skinWidth, controller.maxSlopeAngleDegrees,
                                                       layerMask, self);
        if (probe.hit) {
            controller.groundNormal = probe.normal;
            controller.groundY = probe.point.y;
        } else {
            controller.groundNormal = glm::vec3{0.0f, 1.0f, 0.0f};
            controller.groundY = bottom.y;
        }
        const bool grounded = probe.walkable && controller.state.velocity.y <= 0.0f;
        if (grounded) {
            groundPoint = probe.groundPoint;
            const float gap = bottom.y - probe.groundPoint.y; // 0 when already touching/inside
            if (gap > 0.0f) {
                controller.state.position.y -= gap;
                transform.position = controller.state.position;
            }
        }
        controller.state.grounded = grounded;
    }

    // ------------------------------------------------------------------
    // The shared movement core, ticked directly with the caller's
    // real dt (friction/accelerate). It runs as a velocity oracle:
    // the per-tick ground height is the PROBED surface (resting
    // capsule centre), and while airborne the ground clamp is moved
    // out of reach entirely so the core can never invent a landing
    // the raycast did not see.
    // ------------------------------------------------------------------
    MoveSettings effective = controller.settings;
    effective.groundY = controller.state.grounded ? groundPoint.y + bottomOffset
                                                  : -std::numeric_limits<float>::max();
    InputCmd sequenced = cmd;
    sequenced.cmdNumber = ++controller.cmdCounter;
    playerMove(sequenced, controller.state, effective, dt);

    // ------------------------------------------------------------------
    // Wall clipping & step-up. Two horizontal probes along the
    // velocity — one just above the capsule bottom (low obstacles a
    // centre-height ray would sail over) and one at capsule centre
    // (full-height walls) — find the closest steep (non-walkable)
    // plane in reach. The velocity is then clipped PURELY
    // horizontally against it (PM_ClipVelocity in the XZ plane:
    // vy is never touched), so when PhysicsSystem integrates, there
    // is no wall penetration for its positional solver to resolve —
    // and therefore no mechanism left that could displace the
    // character vertically. What remains is exact wall sliding.
    // The single exception: grounded movement into an obstacle no
    // taller than maxStepHeight steps up onto it (probe straight
    // down ahead, walkable surface within the step limit) instead
    // of clipping. Step-up never applies while airborne.
    // ------------------------------------------------------------------
    glm::vec3& velocity = controller.state.velocity;
    const glm::vec3 horizontal{velocity.x, 0.0f, velocity.z};
    const float horizontalSpeed = glm::length(horizontal);
    if (horizontalSpeed > 1e-6f) {
        const glm::vec3 dir = horizontal / horizontalSpeed;
        const float reach = capsuleRadius + horizontalSpeed * dt + controller.skinWidth;
        const glm::vec3 probeOrigins[2] = {
            controller.state.position + glm::vec3{0.0f, -bottomOffset + 0.02f, 0.0f}, // feet
            controller.state.position,                                                 // centre
        };
        RaycastHit wallHit;
        bool wallFound = false;
        for (const glm::vec3& origin : probeOrigins) {
            RaycastHit candidate;
            if (castFiltered(scene, Ray{origin, dir}, reach, layerMask, self, candidate) &&
                candidate.distance > 1e-4f &&
                !isWalkableNormal(candidate.normal, controller.maxSlopeAngleDegrees) &&
                (!wallFound || candidate.distance < wallHit.distance)) {
                wallHit = candidate;
                wallFound = true;
            }
        }
        if (wallFound) {
            bool stepped = false;
            if (controller.state.grounded) {
                const glm::vec3 ahead = controller.state.position + dir * reach;
                RaycastHit stepHit;
                const glm::vec3 stepOrigin{ahead.x, groundPoint.y + controller.maxStepHeight + controller.skinWidth,
                                           ahead.z};
                const float stepLength = controller.maxStepHeight + 2.0f * controller.skinWidth;
                if (castFiltered(scene, Ray{stepOrigin, glm::vec3{0.0f, -1.0f, 0.0f}}, stepLength, layerMask, self,
                                 stepHit) &&
                    stepHit.distance > 1e-4f && // a real surface, not an inside hit
                    isWalkableNormal(stepHit.normal, controller.maxSlopeAngleDegrees) &&
                    stepHit.position.y > groundPoint.y + 0.002f &&
                    stepHit.position.y <= groundPoint.y + controller.maxStepHeight + 1e-4f) {
                    // Lift AND advance by this tick's displacement:
                    // lifting alone would leave the capsule bottom
                    // sphere straddling the ledge corner, whose
                    // diagonal contact normal shoves the character
                    // back off the step. Advanced past the edge,
                    // the only contact left is the ledge top.
                    controller.state.position.x += horizontal.x * dt;
                    controller.state.position.z += horizontal.z * dt;
                    controller.state.position.y = stepHit.position.y + bottomOffset;
                    transform.position = controller.state.position;
                    controller.groundY = stepHit.position.y;
                    controller.groundNormal = stepHit.normal;
                    stepped = true;
                }
            }
            if (!stepped) {
                velocity = clipVelocityHorizontal(velocity, wallHit.normal, 1.0f);
            }
        }
    }

    // Handshake, part 2: the (clipped) velocity is the rigid body's
    // velocity; the controller keeps owning gravity and sleep so
    // PhysicsSystem never double-integrates either, and its solver
    // only integrates the capsule and resolves real contacts.
    body.velocity = controller.state.velocity;
    body.useGravity = false;
    body.allowSleep = false;
    body.sleeping = false;
    body.sleepTimer = 0.0f;
    return true;
}

bool PlayerSystem::updateCharacter(Scene& scene, const Input& input, double deltaSeconds) {
    const CharacterParts parts = findCharacterPlayer(scene);
    if (parts.controller == nullptr || parts.player.camera == nullptr) {
        return false;
    }
    InputCmd cmd;
    if (input.isKeyDown(Key::W)) {
        cmd.forward += 1.0f;
    }
    if (input.isKeyDown(Key::S)) {
        cmd.forward -= 1.0f;
    }
    if (input.isKeyDown(Key::D)) {
        cmd.strafe += 1.0f;
    }
    if (input.isKeyDown(Key::A)) {
        cmd.strafe -= 1.0f;
    }
    cmd.jump = input.isKeyDown(Key::Space);
    // Movement yaw convention (0 = -Z) from the camera's facing.
    const glm::vec3 front = parts.player.camera->camera.front();
    cmd.yawDegrees = glm::degrees(std::atan2(-front.x, -front.z));
    return driveCharacter(scene, cmd, deltaSeconds);
}

bool PlayerSystem::syncCamera(Scene& scene, float eyeHeight) {
    const PlayerParts player = findPlayer(scene);
    if (player.camera == nullptr || player.transform == nullptr) {
        return false;
    }
    player.camera->camera.setPosition(player.transform->position + glm::vec3{0.0f, eyeHeight, 0.0f});
    return true;
}

} // namespace engine
