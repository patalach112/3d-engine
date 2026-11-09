#include "Geometry/Ray.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <limits>
#include <utility>

namespace engine {

bool rayIntersectsAABB(const Ray& ray, const AABB& box, float& outDistance, glm::vec3& outNormal) {
    // Slab method: the ray's parameter interval inside each axis slab
    // is intersected with the running interval; the box is hit iff the
    // intervals overlap and reach in front of the origin. Axes the ray
    // runs parallel to are handled explicitly (origin must already be
    // inside that slab) instead of multiplying by an infinite inverse
    // direction, which would turn "origin exactly on the slab plane"
    // into NaNs.
    float tNear = 0.0f; // clamped at the origin: an inside start is a 0-distance hit
    float tFar = std::numeric_limits<float>::max();
    int entryAxis = -1;
    float entrySign = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float direction = ray.direction[axis];
        if (std::abs(direction) < 1e-12f) {
            if (ray.origin[axis] < box.min[axis] || ray.origin[axis] > box.max[axis]) {
                return false;
            }
            continue;
        }
        const float invDirection = 1.0f / direction;
        float t0 = (box.min[axis] - ray.origin[axis]) * invDirection;
        float t1 = (box.max[axis] - ray.origin[axis]) * invDirection;
        float sign = -1.0f; // entering through the min face: normal opposes +axis travel
        if (t0 > t1) {
            std::swap(t0, t1);
            sign = 1.0f; // entering through the max face
        }
        if (t0 > tNear) {
            tNear = t0;
            entryAxis = axis;
            entrySign = sign;
        }
        tFar = std::min(tFar, t1);
        if (tNear > tFar) {
            return false;
        }
    }
    if (tFar < 0.0f) {
        return false; // box entirely behind the origin
    }
    outDistance = tNear;
    if (entryAxis >= 0) {
        outNormal = glm::vec3{0.0f};
        outNormal[entryAxis] = entrySign;
    } else {
        outNormal = -ray.direction; // origin inside the box: no entry face
    }
    return true;
}

bool rayIntersectsTriangle(const Ray& ray, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                           float& outDistance, glm::vec2& outBarycentric) {
    // Möller–Trumbore: solves origin + t*d = a + u*(b-a) + v*(c-a)
    // directly for (t, u, v) via Cramer's rule. The determinant's
    // sign encodes facing; taking its absolute value (and dividing
    // through) makes the test two-sided without a branch on winding.
    const glm::vec3 edge1 = b - a;
    const glm::vec3 edge2 = c - a;
    const glm::vec3 pvec = glm::cross(ray.direction, edge2);
    const float determinant = glm::dot(edge1, pvec);
    if (std::abs(determinant) < 1e-12f) {
        return false; // ray parallel to the triangle plane, or degenerate triangle
    }
    const float invDeterminant = 1.0f / determinant;
    const glm::vec3 tvec = ray.origin - a;
    const float u = glm::dot(tvec, pvec) * invDeterminant;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::vec3 qvec = glm::cross(tvec, edge1);
    const float v = glm::dot(ray.direction, qvec) * invDeterminant;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = glm::dot(edge2, qvec) * invDeterminant;
    if (t < 0.0f) {
        return false; // intersection behind the origin
    }
    outDistance = t;
    outBarycentric = glm::vec2{u, v};
    return true;
}

Ray rayThroughNdc(glm::vec2 ndc, const glm::mat4& inverseViewProjection) {
    const glm::vec4 nearClip{ndc.x, ndc.y, 0.0f, 1.0f};
    const glm::vec4 farClip{ndc.x, ndc.y, 1.0f, 1.0f};
    const glm::vec4 nearWorld4 = inverseViewProjection * nearClip;
    const glm::vec4 farWorld4 = inverseViewProjection * farClip;
    const glm::vec3 nearWorld = glm::vec3{nearWorld4} / nearWorld4.w;
    const glm::vec3 farWorld = glm::vec3{farWorld4} / farWorld4.w;
    Ray ray;
    ray.origin = nearWorld;
    const glm::vec3 direction = farWorld - nearWorld;
    const float length = glm::length(direction);
    ray.direction = length > 1e-12f ? direction / length : glm::vec3{0.0f, 0.0f, -1.0f};
    return ray;
}

glm::vec2 screenToNdc(glm::vec2 pixelPosition, glm::vec2 viewportSize) {
    // Vulkan NDC and window pixel coordinates share the same y-down
    // orientation, so both axes map with the same formula (no y flip:
    // the renderer's projection Y-flip already accounts for it).
    const float x = viewportSize.x > 0.0f ? 2.0f * pixelPosition.x / viewportSize.x - 1.0f : 0.0f;
    const float y = viewportSize.y > 0.0f ? 2.0f * pixelPosition.y / viewportSize.y - 1.0f : 0.0f;
    return glm::vec2{x, y};
}

} // namespace engine
