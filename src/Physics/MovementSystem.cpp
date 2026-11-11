#include "Physics/MovementSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

    // Sprint (Source aux-power model): the wish-speed cap rises to
    // sprintSpeed only while the sprint input is held AND the
    // player is actually moving, and the tank drains for exactly
    // those ticks. Exhaustion latches until the tank recovers to
    // 25%, so sprint can never be feathered at empty.
    const bool sprinting = cmd.sprint && wishLength > 1e-6f && !state.sprintExhausted && state.stamina > 0.0f;
    if (sprinting) {
        state.stamina = std::max(0.0f, state.stamina - settings.staminaDrainPerSecond * dt);
        if (state.stamina <= 0.0f) {
            state.sprintExhausted = true;
        }
    } else {
        state.stamina = std::min(1.0f, state.stamina + settings.staminaRegenPerSecond * dt);
        if (state.sprintExhausted && state.stamina >= 0.25f) {
            state.sprintExhausted = false;
        }
    }

    float wishSpeed = 0.0f;
    if (wishLength > 1e-6f) {
        wishDir /= wishLength;
        wishSpeed = std::min(wishLength, 1.0f) * (sprinting ? settings.sprintSpeed : settings.maxSpeed);
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

namespace {

// ---------------------------------------------------------------------------
// Exact segment/box distance machinery (box-local space), the narrow
// phase of the capsule trace. Distances are composed from the 12
// face triangles of the box, which keeps every closest pair exact:
// segment vs triangle = endpoint-to-triangle, vertex-to-segment,
// and edge-to-edge segment pairs.
// ---------------------------------------------------------------------------

float pointSegmentDistance(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b, glm::vec3& outOnSegment) {
    const glm::vec3 ab = b - a;
    const float lengthSq = glm::dot(ab, ab);
    const float t = lengthSq > 1e-12f ? glm::clamp(glm::dot(point - a, ab) / lengthSq, 0.0f, 1.0f) : 0.0f;
    outOnSegment = a + ab * t;
    return glm::length(point - outOnSegment);
}

// Ericson's segment/segment closest points (Robustness: degenerate
// segments degrade to point tests).
float segmentSegmentDistance(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2,
                             glm::vec3& outOnFirst, glm::vec3& outOnSecond) {
    const glm::vec3 d1 = q1 - p1;
    const glm::vec3 d2 = q2 - p2;
    const glm::vec3 r = p1 - p2;
    const float a = glm::dot(d1, d1);
    const float e = glm::dot(d2, d2);
    const float f = glm::dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f) {
        outOnFirst = p1;
        outOnSecond = p2;
        return glm::length(r);
    }
    if (a <= 1e-12f) {
        t = glm::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = glm::dot(d1, r);
        if (e <= 1e-12f) {
            s = glm::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > 1e-12f ? glm::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = glm::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = glm::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    outOnFirst = p1 + d1 * s;
    outOnSecond = p2 + d2 * t;
    return glm::length(outOnFirst - outOnSecond);
}

// Closest point on a triangle to a point (Ericson, RTCD 5.1.5).
glm::vec3 closestPointOnTriangle(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = point - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
        return a;
    const glm::vec3 bp = point - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
        return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        return a + ab * (d1 / (d1 - d3));
    const glm::vec3 cp = point - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
        return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

float segmentTriangleDistance(const glm::vec3& segA, const glm::vec3& segB, const glm::vec3& a, const glm::vec3& b,
                              const glm::vec3& c, glm::vec3& outOnSegment, glm::vec3& outOnTriangle) {
    float best = std::numeric_limits<float>::max();
    glm::vec3 candidateSeg;
    glm::vec3 candidateTri;
    const auto consider = [&](float distance, const glm::vec3& onSeg, const glm::vec3& onTri) {
        if (distance < best) {
            best = distance;
            candidateSeg = onSeg;
            candidateTri = onTri;
        }
    };
    glm::vec3 onTri = closestPointOnTriangle(segA, a, b, c);
    consider(glm::length(segA - onTri), segA, onTri);
    onTri = closestPointOnTriangle(segB, a, b, c);
    consider(glm::length(segB - onTri), segB, onTri);
    glm::vec3 onSeg;
    float distance = pointSegmentDistance(a, segA, segB, onSeg);
    consider(distance, onSeg, a);
    distance = pointSegmentDistance(b, segA, segB, onSeg);
    consider(distance, onSeg, b);
    distance = pointSegmentDistance(c, segA, segB, onSeg);
    consider(distance, onSeg, c);
    glm::vec3 onFirst;
    glm::vec3 onSecond;
    distance = segmentSegmentDistance(segA, segB, a, b, onFirst, onSecond);
    consider(distance, onFirst, onSecond);
    distance = segmentSegmentDistance(segA, segB, b, c, onFirst, onSecond);
    consider(distance, onFirst, onSecond);
    distance = segmentSegmentDistance(segA, segB, c, a, onFirst, onSecond);
    consider(distance, onFirst, onSecond);
    outOnSegment = candidateSeg;
    outOnTriangle = candidateTri;
    return best;
}

// Segment vs AABB (half extents about the origin) in box-local
// space. On intersection the distance is 0 and both closest points
// are the segment's entry point (a defined, usable contact point).
float segmentBoxDistanceLocal(const glm::vec3& segA, const glm::vec3& segB, const glm::vec3& half,
                              glm::vec3& outOnSegment, glm::vec3& outOnBox) {
    // Slab intersection test (also yields the entry point).
    {
        const glm::vec3 dir = segB - segA;
        float tMin = 0.0f;
        float tMax = 1.0f;
        bool intersects = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(dir[axis]) <= 1e-12f) {
                if (std::abs(segA[axis]) > half[axis])
                    intersects = false;
                continue;
            }
            float t1 = (-half[axis] - segA[axis]) / dir[axis];
            float t2 = (half[axis] - segA[axis]) / dir[axis];
            if (t1 > t2)
                std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax)
                intersects = false;
        }
        if (intersects) {
            outOnSegment = segA + dir * tMin;
            outOnBox = outOnSegment;
            return 0.0f;
        }
    }
    // 12 face triangles (CCW winding is irrelevant for distance).
    const glm::vec3 corners[8] = {
        glm::vec3{-half.x, -half.y, -half.z}, glm::vec3{half.x, -half.y, -half.z},
        glm::vec3{half.x, -half.y, half.z},   glm::vec3{-half.x, -half.y, half.z},
        glm::vec3{-half.x, half.y, -half.z},  glm::vec3{half.x, half.y, -half.z},
        glm::vec3{half.x, half.y, half.z},    glm::vec3{-half.x, half.y, half.z},
    };
    static constexpr int triangles[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 1}, {1, 4, 5},
                                             {1, 5, 2}, {2, 5, 6}, {2, 6, 3}, {3, 6, 7}, {3, 7, 0}, {0, 7, 4}};
    float best = std::numeric_limits<float>::max();
    for (const auto& tri : triangles) {
        glm::vec3 onSeg;
        glm::vec3 onTri;
        const float distance = segmentTriangleDistance(segA, segB, corners[tri[0]], corners[tri[1]], corners[tri[2]],
                                                       onSeg, onTri);
        if (distance < best) {
            best = distance;
            outOnSegment = onSeg;
            outOnBox = onTri;
        }
    }
    return best;
}

} // namespace

float capsuleBoxDistance(const glm::vec3& segmentA, const glm::vec3& segmentB, float radius,
                         const glm::vec3& boxCenter, const glm::mat3& boxAxes, const glm::vec3& boxHalfExtents,
                         glm::vec3* outSegmentPoint, glm::vec3* outBoxPoint) {
    const glm::mat3 worldToLocal = glm::transpose(boxAxes);
    const glm::vec3 localA = worldToLocal * (segmentA - boxCenter);
    const glm::vec3 localB = worldToLocal * (segmentB - boxCenter);
    glm::vec3 localSeg;
    glm::vec3 localBox;
    const float segmentDistance = segmentBoxDistanceLocal(localA, localB, glm::abs(boxHalfExtents), localSeg, localBox);
    if (outSegmentPoint != nullptr)
        *outSegmentPoint = boxCenter + boxAxes * localSeg;
    if (outBoxPoint != nullptr)
        *outBoxPoint = boxCenter + boxAxes * localBox;
    return segmentDistance - radius;
}

bool sweepCapsule(const glm::vec3& startCenter, const glm::vec3& displacement, float radius, float halfHeight,
                  std::span<const TraceBox> boxes, SweepHit& outHit) {
    constexpr float kContactTolerance = 0.001f; // 1 mm: "touching"
    const float displacementLength = glm::length(displacement);
    const glm::vec3 segmentOffset{0.0f, halfHeight, 0.0f};

    bool found = false;
    SweepHit best;
    for (size_t index = 0; index < boxes.size(); ++index) {
        const TraceBox& box = boxes[index];
        const auto distanceAt = [&](const glm::vec3& center, glm::vec3& onCapsule, glm::vec3& onBox) {
            return capsuleBoxDistance(center - segmentOffset, center + segmentOffset, radius, box.center, box.axes,
                                      box.halfExtents, &onCapsule, &onBox);
        };

        glm::vec3 onCapsule;
        glm::vec3 onBox;
        const float startDistance = distanceAt(startCenter, onCapsule, onBox);
        if (startDistance < -1e-6f) {
            // Penetrating: report the shallowest exit. When the
            // segment itself has entered the box (distance pinned
            // at -radius), exit along the least-penetrated axis.
            SweepHit penetration;
            penetration.fraction = 0.0f;
            penetration.boxIndex = static_cast<int32_t>(index);
            penetration.startPenetrated = true;
            const glm::vec3 segA = startCenter - segmentOffset;
            const glm::vec3 segB = startCenter + segmentOffset;
            glm::vec3 segPoint;
            glm::vec3 boxPoint;
            const float segmentDistance =
                capsuleBoxDistance(segA, segB, radius, box.center, box.axes, box.halfExtents, &segPoint, &boxPoint) +
                radius;
            if (segmentDistance > 1e-6f) {
                penetration.penetrationNormal = glm::normalize(segPoint - boxPoint);
                penetration.penetrationDepth = radius - segmentDistance;
            } else {
                const glm::mat3 worldToLocal = glm::transpose(box.axes);
                const glm::vec3 local = worldToLocal * (startCenter - box.center);
                const glm::vec3 half = glm::abs(box.halfExtents);
                const glm::vec3 exit = half - glm::abs(local) + glm::vec3{radius};
                int axis = 0;
                if (exit.y < exit.x)
                    axis = 1;
                if (exit.z < exit[axis])
                    axis = 2;
                glm::vec3 localNormal{0.0f};
                localNormal[axis] = local[axis] >= 0.0f ? 1.0f : -1.0f;
                penetration.penetrationNormal = box.axes * localNormal;
                penetration.penetrationDepth = exit[axis];
            }
            penetration.normal = penetration.penetrationNormal;
            penetration.point = boxPoint;
            if (!found || penetration.penetrationDepth > best.penetrationDepth) {
                best = penetration;
                found = true;
            }
            continue;
        }
        if (displacementLength <= 1e-8f) {
            continue; // no motion: only penetration matters
        }
        if (startDistance <= kContactTolerance) {
            // Resting/tangent contact: an impact only when the
            // motion actually approaches along the surface normal.
            glm::vec3 normal = onCapsule - onBox;
            normal = glm::length(normal) > 1e-8f ? glm::normalize(normal) : -displacement / displacementLength;
            if (glm::dot(displacement, normal) < -1e-6f) { // genuine approach only (see header note)
                SweepHit hit;
                hit.fraction = 0.0f;
                hit.normal = normal;
                hit.point = onBox;
                hit.boxIndex = static_cast<int32_t>(index);
                if (!found || (best.startPenetrated == false && hit.fraction < best.fraction)) {
                    best = hit;
                    found = true;
                }
            }
            continue;
        }
        // Conservative advancement: the distance can shrink at most
        // |displacement| per unit fraction, so stepping by the
        // current distance can never overshoot the surface.
        float fraction = 0.0f;
        bool hitFound = false;
        glm::vec3 hitNormal{0.0f, 1.0f, 0.0f};
        glm::vec3 hitPoint{0.0f};
        for (int iteration = 0; iteration < 8; ++iteration) {
            const glm::vec3 center = startCenter + displacement * fraction;
            glm::vec3 atCapsule;
            glm::vec3 atBox;
            const float distance = distanceAt(center, atCapsule, atBox);
            if (distance <= kContactTolerance) {
                glm::vec3 normal = atCapsule - atBox;
                hitNormal = glm::length(normal) > 1e-8f ? glm::normalize(normal) : -displacement / displacementLength;
                hitPoint = atBox;
                hitFound = glm::dot(displacement, hitNormal) < -1e-6f || fraction > 0.0f;
                break;
            }
            fraction += distance / displacementLength;
            if (fraction >= 1.0f || (found && !best.startPenetrated && fraction >= best.fraction)) {
                break;
            }
        }
        if (hitFound && fraction < 1.0f) {
            SweepHit hit;
            hit.fraction = std::max(fraction, 0.0f);
            hit.normal = hitNormal;
            hit.point = hitPoint;
            hit.boxIndex = static_cast<int32_t>(index);
            if (!found || (!best.startPenetrated && hit.fraction < best.fraction)) {
                best = hit;
                found = true;
            }
        }
    }
    if (found) {
        outHit = best;
    }
    return found;
}

} // namespace engine
