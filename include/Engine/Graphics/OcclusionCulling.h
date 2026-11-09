#pragma once

#include "Geometry/Bounds.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <span>

namespace engine {

// ---------------------------------------------------------------------------
// Conservative screen-space occlusion test (Phase 21).
//
// A candidate AABB is occluded by an occluder AABB when, projected
// into NDC, the candidate's screen rectangle is fully covered by the
// occluder's rectangle AND the candidate's nearest depth is behind
// the occluder's farthest depth. Both conditions together make the
// test sound (it never culls something visible) at the price of
// missing partial or grazing occlusion — the standard trade for a
// cheap CPU pre-pass. Anything touching the camera (a corner behind
// the eye, or crossing the near plane) is never considered occluded.
// ---------------------------------------------------------------------------
namespace occlusion_detail {

struct ProjectedBox {
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    float minZ = 0.0f; // nearest NDC depth (Vulkan: [0, 1], smaller is closer)
    float maxZ = 0.0f;
};

// Projects the 8 corners; false when any corner is behind the camera
// or in front of the near plane (conservative bail-outs).
inline bool projectBox(const glm::mat4& viewProjection, const AABB& box, ProjectedBox& out) {
    bool first = true;
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{(corner & 1) != 0 ? box.max.x : box.min.x,
                              (corner & 2) != 0 ? box.max.y : box.min.y,
                              (corner & 4) != 0 ? box.max.z : box.min.z};
        const glm::vec4 clip = viewProjection * glm::vec4{point, 1.0f};
        if (clip.w <= 1e-6f) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3{clip} / clip.w;
        if (ndc.z < 0.0f) {
            return false;
        }
        if (first) {
            out = ProjectedBox{ndc.x, ndc.x, ndc.y, ndc.y, ndc.z, ndc.z};
            first = false;
        } else {
            out.minX = glm::min(out.minX, ndc.x);
            out.maxX = glm::max(out.maxX, ndc.x);
            out.minY = glm::min(out.minY, ndc.y);
            out.maxY = glm::max(out.maxY, ndc.y);
            out.minZ = glm::min(out.minZ, ndc.z);
            out.maxZ = glm::max(out.maxZ, ndc.z);
        }
    }
    return true;
}

} // namespace occlusion_detail

[[nodiscard]] inline bool isOccludedBy(const glm::mat4& viewProjection, const AABB& occluder,
                                       const AABB& candidate) {
    occlusion_detail::ProjectedBox occluderBox;
    occlusion_detail::ProjectedBox candidateBox;
    if (!occlusion_detail::projectBox(viewProjection, occluder, occluderBox) ||
        !occlusion_detail::projectBox(viewProjection, candidate, candidateBox)) {
        return false;
    }
    const bool covered = candidateBox.minX >= occluderBox.minX && candidateBox.maxX <= occluderBox.maxX &&
                         candidateBox.minY >= occluderBox.minY && candidateBox.maxY <= occluderBox.maxY;
    const bool behind = candidateBox.minZ > occluderBox.maxZ;
    return covered && behind;
}

[[nodiscard]] inline bool isOccludedByAny(const glm::mat4& viewProjection, std::span<const AABB> occluders,
                                          const AABB& candidate) {
    for (const AABB& occluder : occluders) {
        if (isOccludedBy(viewProjection, occluder, candidate)) {
            return true;
        }
    }
    return false;
}

} // namespace engine
