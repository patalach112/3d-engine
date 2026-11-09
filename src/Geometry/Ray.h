#pragma once
#include "Core/Export.h"

#include "Geometry/Bounds.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace engine {

// ---------------------------------------------------------------------------
// Ray
//
// A world-space picking/visibility ray: an origin and a *normalized*
// direction. Distances reported by the intersection tests below are in
// world units along the ray (origin + direction * distance), which is
// only true while direction stays unit-length — constructors in this
// module normalize for you, and Scene::raycast assumes the same.
// ---------------------------------------------------------------------------
struct ENGINE_GEOMETRY_API Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};

    [[nodiscard]] glm::vec3 at(float distance) const { return origin + direction * distance; }
};

// Slab-method ray vs axis-aligned box test (the box side of picking:
// mesh local bounds and box colliders are both tested through this).
// On a hit, outDistance is the entry distance (>= 0; exactly 0 when the
// origin is inside the box) and outNormal is the unit face normal of
// the entry face, oriented against the ray. When the origin is inside
// the box there is no entry face, so outNormal is -direction (the face
// the ray is looking away from) — callers that need a surface normal
// should treat an inside hit as "already overlapping". Returns false
// when the ray misses or the box lies entirely behind the origin.
[[nodiscard]] ENGINE_GEOMETRY_API bool rayIntersectsAABB(const Ray& ray, const AABB& box, float& outDistance,
                                                         glm::vec3& outNormal);

// Möller–Trumbore ray vs triangle test, two-sided (picking must hit
// back faces: a mesh viewed from inside or with flipped winding is
// still the surface under the cursor). On a hit, outDistance is the
// ray parameter of the intersection and outBarycentric holds the
// (u, v) weights of vertices b and c, so the hit point is
// a + u*(b-a) + v*(c-a). Degenerate (zero-area) triangles never hit.
[[nodiscard]] ENGINE_GEOMETRY_API bool rayIntersectsTriangle(const Ray& ray, const glm::vec3& a, const glm::vec3& b,
                                                             const glm::vec3& c, float& outDistance,
                                                             glm::vec2& outBarycentric);

// Builds a world-space ray through the given normalized device
// coordinates by unprojecting the near and far clip-space points with
// the inverse view-projection matrix. NDC follows the renderer's
// Vulkan convention: x in [-1, 1] left-to-right, y in [-1, 1]
// *top-to-bottom* (the projection's Y flip is already accounted for
// by the inverse), z 0 at the near plane and 1 at the far plane. The
// returned origin sits on the near plane; camera-facing helpers snap
// it to the eye position when exact eye-relative distances matter.
[[nodiscard]] ENGINE_GEOMETRY_API Ray rayThroughNdc(glm::vec2 ndc, const glm::mat4& inverseViewProjection);

// Converts a pixel position inside a viewport (origin top-left, y
// growing downward — window/mouse coordinates) to the NDC convention
// described above. A zero-sized viewport dimension maps to 0 on that
// axis instead of dividing by zero.
[[nodiscard]] ENGINE_GEOMETRY_API glm::vec2 screenToNdc(glm::vec2 pixelPosition, glm::vec2 viewportSize);

} // namespace engine
