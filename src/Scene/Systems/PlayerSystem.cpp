#include "Scene/Systems/PlayerSystem.h"

#include "Platform/Input.h"
#include "Scene/Raycast.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
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
    // resumes from. Groundedness is NOT read from it (below). The
    // same position seeds render interpolation: hosts blend from
    // here to the post-step transform with the frame's alpha.
    controller.state.position = transform.position;
    controller.interpolationFrom = transform.position;
    controller.interpolationValid = true;

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
    // The sweep below owns this tick's integration: capture the
    // pre-move position so the capsule trace starts from where the
    // character actually is (playerMove's own position integration
    // serves the network simulations, where no transform exists;
    // here its velocity is what we consume).
    glm::vec3 sweepOrigin = controller.state.position;
    playerMove(sequenced, controller.state, effective, dt);

    // ------------------------------------------------------------------
    // Step-up trigger. Two horizontal probes along the velocity —
    // one just above the capsule bottom (low obstacles a centre-
    // height ray would sail over) and one at capsule centre — look
    // for a steep plane in reach. When grounded movement runs into
    // an obstacle no taller than maxStepHeight, the character
    // steps up onto it (probe straight down ahead, walkable
    // surface within the step limit) instead of being stopped by
    // the sweep below. Step-up never applies while airborne, and
    // it is the ONLY vertical assistance: everything else that
    // blocks the capsule is handled by the sweep's clipping.
    // ------------------------------------------------------------------
    {
        const glm::vec3 horizontal{controller.state.velocity.x, 0.0f, controller.state.velocity.z};
        const float horizontalSpeed = glm::length(horizontal);
        if (horizontalSpeed > 1e-6f && controller.state.grounded) {
            const glm::vec3 dir = horizontal / horizontalSpeed;
            const float reach = capsuleRadius + horizontalSpeed * dt + controller.skinWidth;
            const glm::vec3 probeOrigins[2] = {
                controller.state.position + glm::vec3{0.0f, -bottomOffset + 0.02f, 0.0f}, // feet
                controller.state.position,                                                 // centre
            };
            bool wallFound = false;
            for (const glm::vec3& origin : probeOrigins) {
                RaycastHit candidate;
                if (castFiltered(scene, Ray{origin, dir}, reach, layerMask, self, candidate) &&
                    candidate.distance > 1e-4f &&
                    !isWalkableNormal(candidate.normal, controller.maxSlopeAngleDegrees)) {
                    wallFound = true;
                    break;
                }
            }
            if (wallFound) {
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
                    // The step is part of this tick's path: the
                    // sweep resumes from the stepped position (it
                    // still collision-checks the remaining motion).
                    sweepOrigin = controller.state.position;
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Capsule-trace sweep (CCD): this tick's actual motion. Nearby
    // box/plane colliders become trace geometry; any starting
    // penetration is resolved ONCE along the shallowest exit
    // (bounded, so a bad spawn can never accumulate solver energy);
    // then the capsule bumps and slides: sweep along the velocity,
    // stop exactly at t_hit, PM_ClipVelocity against the impact
    // plane (full clip on floors/ceilings, purely horizontal on
    // walls so sliding can never manufacture lift), re-sweep the
    // remaining fraction. The rigid-body solver is out of this
    // loop entirely (PhysicsSystem treats the body as immovable),
    // so there is no penetration feedback left to launch anyone.
    // ------------------------------------------------------------------
    {
        struct TracePlane {
            glm::vec3 normal{0.0f, 1.0f, 0.0f};
            float offset = 0.0f;
        };
        std::vector<TraceBox> boxes;
        std::vector<TracePlane> planes;
        // Gather around the path the sweep will actually travel:
        // sweepOrigin -> sweepOrigin + velocity*dt. (controller.
        // state.position has already been integrated forward by
        // playerMove; centring the query there would reject the
        // very walls the sweep is about to hit.)
        const glm::vec3 sweepStart = sweepOrigin;
        const glm::vec3 sweepEnd = sweepStart + controller.state.velocity * dt;
        const float pathRadius = capsuleRadius + glm::length(controller.state.velocity) * dt + controller.skinWidth;
        auto& registry = scene.registry();
        for (const entt::entity entity : registry.view<TransformComponent, ColliderComponent>()) {
            if (entity == self) {
                continue;
            }
            const ColliderComponent& other = registry.get<ColliderComponent>(entity);
            if (other.isTrigger || !shouldCollide(collider, other)) {
                continue;
            }
            const TransformComponent& otherTransform = registry.get<TransformComponent>(entity);
            if (other.type == ColliderComponent::Type::Plane) {
                const glm::vec3 normal = glm::length(other.planeNormal) > 1e-8f
                                             ? glm::normalize(other.planeNormal)
                                             : glm::vec3{0.0f, 1.0f, 0.0f};
                planes.push_back(TracePlane{normal, other.planeOffset});
                continue;
            }
            if (other.type != ColliderComponent::Type::Box && other.type != ColliderComponent::Type::Obb) {
                continue; // spheres/capsules/hulls: solver reports their contacts; the trace world is boxes+planes
            }
            // Cheap reject: candidate bounding sphere vs the swept path.
            const float candidateRadius = glm::length(glm::abs(other.halfExtents * otherTransform.scale)) +
                                          glm::length(other.centerOffset * otherTransform.scale);
            const glm::vec3 pathMid = (sweepStart + sweepEnd) * 0.5f;
            if (glm::length(otherTransform.position - pathMid) > candidateRadius + pathRadius) {
                continue;
            }
            TransformComponent oriented = otherTransform;
            if (!oriented.useQuaternionOrientation) {
                oriented.syncOrientationFromEuler();
            }
            if (const RigidBodyComponent* otherBody = registry.try_get<RigidBodyComponent>(entity)) {
                if (otherBody->orientationInitialized) {
                    oriented.orientation = otherBody->orientation;
                }
            }
            TraceBox box;
            box.axes = glm::mat3_cast(oriented.orientation);
            box.center = otherTransform.position + box.axes * (other.centerOffset * otherTransform.scale);
            box.halfExtents = glm::abs(other.halfExtents * otherTransform.scale);
            boxes.push_back(box);
        }

        glm::vec3 position = sweepOrigin;
        glm::vec3 velocity = controller.state.velocity;

        // Bounded depenetration: at most two shallowest-exit pushes
        // per tick, each capped, so overlap from spawns or shoves
        // resolves monotonically instead of feeding a solver.
        for (int pass = 0; pass < 2; ++pass) {
            SweepHit penetration;
            if (!sweepCapsule(position, glm::vec3{0.0f}, capsuleRadius, segmentHalf, boxes, penetration) ||
                !penetration.startPenetrated) {
                break;
            }
            position += penetration.penetrationNormal * std::min(penetration.penetrationDepth, 0.5f);
        }

        // Bump-and-slide (Source TryPlayerMove shape, max 3 bumps).
        float timeLeft = 1.0f;
        for (int bump = 0; bump < 3 && timeLeft > 1e-6f; ++bump) {
            const glm::vec3 displacement = velocity * (dt * timeLeft);
            const float displacementLength = glm::length(displacement);
            if (displacementLength <= 1e-8f) {
                break;
            }
            SweepHit hit;
            const bool boxHit = sweepCapsule(position, displacement, capsuleRadius, segmentHalf, boxes, hit);
            // Analytic plane sweep, competing for earliest impact.
            float bestFraction = boxHit ? hit.fraction : 1.0f;
            glm::vec3 bestNormal = boxHit ? hit.normal : glm::vec3{0.0f, 1.0f, 0.0f};
            bool haveHit = boxHit;
            for (const TracePlane& plane : planes) {
                const float approach = glm::dot(displacement, plane.normal);
                if (approach >= 0.0f) {
                    continue;
                }
                const glm::vec3 segA = position - glm::vec3{0.0f, segmentHalf, 0.0f};
                const glm::vec3 segB = position + glm::vec3{0.0f, segmentHalf, 0.0f};
                const float distance =
                    std::min(glm::dot(plane.normal, segA), glm::dot(plane.normal, segB)) - plane.offset - capsuleRadius;
                if (distance < -1e-4f) {
                    continue; // already through: depenetration's problem, not the sweep's
                }
                const float fraction = distance <= 0.0f ? 0.0f : distance / -approach;
                if (fraction < bestFraction) {
                    bestFraction = fraction;
                    bestNormal = plane.normal;
                    haveHit = true;
                }
            }
            if (!haveHit || bestFraction >= 1.0f) {
                position += displacement;
                timeLeft = 0.0f;
                break;
            }
            // Stop exactly at the impact point (the trace's 1 mm
            // tolerance leaves the capsule resting at the surface),
            // clip, and re-sweep what is left of the tick.
            position += displacement * bestFraction;
            if (isWalkableNormal(bestNormal, controller.maxSlopeAngleDegrees) ||
                isWalkableNormal(-bestNormal, controller.maxSlopeAngleDegrees)) {
                velocity = clipVelocity(velocity, bestNormal, 1.0f); // floors & ceilings
            } else {
                velocity = clipVelocityHorizontal(velocity, bestNormal, 1.0f); // walls: slide, never lift
            }
            timeLeft *= 1.0f - bestFraction;
            if (glm::length(velocity) <= 1e-6f) {
                break; // the clip(s) consumed all motion (head-on stop / wedged corner)
            }
            // A fraction-0 (resting) hit does NOT end the bump
            // loop: the clipped velocity no longer approaches, so
            // the re-sweep applies the tangential remainder. The
            // 3-bump bound keeps pathological stacks finite.
        }

        controller.state.position = position;
        controller.state.velocity = velocity;
        transform.position = position;
    }

    // Handshake, part 2: the swept velocity is the rigid body's
    // velocity (contacts use it to push dynamic bodies); the
    // controller keeps owning gravity and sleep, and PhysicsSystem
    // treats the body as immovable — it never integrates or
    // corrects a character-driven capsule.
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
    cmd.sprint = input.isKeyDown(Key::LeftShift) || input.isKeyDown(Key::RightShift);
    // Movement yaw convention (0 = -Z) from the camera's facing.
    const glm::vec3 front = parts.player.camera->camera.front();
    cmd.yawDegrees = glm::degrees(std::atan2(-front.x, -front.z));
    return driveCharacter(scene, cmd, deltaSeconds);
}

bool PlayerSystem::syncCameraInterpolated(Scene& scene, float alpha, float eyeHeight) {
    const CharacterParts parts = findCharacterPlayer(scene);
    if (parts.controller == nullptr || parts.player.camera == nullptr || parts.player.transform == nullptr ||
        !parts.controller->interpolationValid) {
        return syncCamera(scene, eyeHeight);
    }
    // Blend the capsule centre between the last two fixed steps.
    // alpha 0 = previous step's end, 1 = the live transform; the
    // GameLoop produces alpha at render rate, so at 1000 fps the
    // camera glides through ~16 distinct blends per physics step
    // instead of snapping at 60 Hz.
    const float t = std::clamp(alpha, 0.0f, 1.0f);
    const glm::vec3 position = glm::mix(parts.controller->interpolationFrom, parts.player.transform->position, t);
    parts.player.camera->camera.setPosition(position + glm::vec3{0.0f, eyeHeight, 0.0f});
    return true;
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
