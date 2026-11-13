#pragma once
#include "Core/Export.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace engine {

// ---------------------------------------------------------------------------
// Directional shadow mapping (pure math half).
//
// One fixed directional key light (the same direction the fragment shader
// lights with) renders the scene's depth from its point of view into a
// square depth texture; the main pass then reprojects each fragment into
// light space and compares depths, so cubes ground themselves on the
// floor and on each other instead of floating. The Vulkan resources live
// in Renderer; everything CPU-testable (light placement, the ortho light
// frustum) lives here.
// ---------------------------------------------------------------------------

// Shadow map resolution (square). 2048 keeps texel size near a centimeter
// over the sandbox frustum — crisp edges at negligible cost.
inline constexpr uint32_t kShadowMapSize = 2048;

// Direction TOWARD the key light (matches cube.frag's lighting).
[[nodiscard]] inline glm::vec3 directionalLightDirection() {
    return glm::normalize(glm::vec3{0.35f, 0.8f, 0.45f});
}

// World-space center the light frustum is aimed at (the sandbox action
// sits between the terrestrial floor and the zero-G cluster at z = -5).
[[nodiscard]] inline glm::vec3 shadowSceneCenter() {
    return glm::vec3{0.0f, 0.0f, -2.5f};
}

// Ortho half-extent of the light frustum: covers the 9x9 floor and the
// whole zero-G cluster from the light's point of view.
inline constexpr float kShadowSceneExtent = 9.0f;
inline constexpr float kShadowLightDistance = 18.0f;
inline constexpr float kShadowNearPlane = 1.0f;
inline constexpr float kShadowFarPlane = 40.0f;

// Light-space view-projection matrix: camera positioned at
// center + direction * distance looking at the center, with an
// orthographic frustum of half-extent `extent`. A point's projected z
// grows with its distance from the light, so a fragment is shadowed when
// its light-space depth exceeds the depth stored from the light's view.
[[nodiscard]] ENGINE_GRAPHICS_API glm::mat4 directionalLightViewProjection(const glm::vec3& lightDirection,
                                                                           const glm::vec3& center, float extent,
                                                                           float lightDistance, float nearPlane,
                                                                           float farPlane);

// ---------------------------------------------------------------------------
// Cascaded shadow maps (Phase 21)
//
// The single fixed light frustum above aliases badly: one 2048 map
// spread over the whole scene gives centimeter texels everywhere.
// CSM splits the camera frustum into depth slices, each with its own
// light view-projection fitted tightly around the slice — texel
// density stays roughly constant with distance, so near geometry
// gets crisp shadows and far geometry keeps coverage. The renderer
// renders one depth target per cascade; the fragment shader picks
// the cascade by view-space depth.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kMaxShadowCascades = 4;

struct ShadowCascade {
    glm::mat4 lightViewProj{1.0f};
    // View-space depth (positive, in front of the camera) up to
    // which this cascade applies.
    float splitFarDepth = 0.0f;
};

// Practical split scheme: a blend (splitLambda 0 = uniform,
// 1 = logarithmic) of the two classic distributions. Entry i is the
// view-space far depth of cascade i; entries at and beyond
// cascadeCount are pinned to farPlane.
[[nodiscard]] ENGINE_GRAPHICS_API std::array<float, kMaxShadowCascades>
computeCascadeSplits(float nearPlane, float farPlane, uint32_t cascadeCount, float splitLambda);

// ---------------------------------------------------------------------------
// Spot light shadows (Phase 32)
//
// One shadow-casting spot gets a dedicated 2D depth map rendered
// from the light's point of view with a perspective projection
// whose field of view is the spot's outer cone. The main pass
// reprojects the fragment into that space (perspective divide)
// and PCF-compares depths, mirroring the directional path. The
// matrix below is the testable core; the Vulkan target lives in
// Renderer.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kSpotShadowMapSize = 1024;

// Light-space view-projection for a spot at `position` shining
// along `direction` (unit length not required): perspective FOV
// = 2 * outerConeRadians, square aspect, Vulkan clip conventions
// (depth 0..1, flipped Y) matching the camera projection.
[[nodiscard]] ENGINE_GRAPHICS_API glm::mat4 spotShadowViewProjection(const glm::vec3& position,
                                                                     const glm::vec3& direction,
                                                                     float outerConeRadians, float nearPlane,
                                                                     float farPlane);

// ---------------------------------------------------------------------------
// Point light shadows (cube shadow mapping)
//
// A shadow-casting point light renders the scene depth six times,
// once per 90-degree cube face, into a 6-layer depth array; the
// fragment shader picks the face by the dominant axis of the
// fragment-to-light vector and PCF-compares in that face's space —
// the same reprojection math as the spot path, so dynamic props
// both cast into and receive from the point's shadow volume. One
// cube is budgeted (the nearest shadow-casting point wins), at
// 512px per face: horror scenes light small rooms, and the bounded
// cost is what keeps shadow rendering micro-stutter-free.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kPointShadowMapSize = 512;
inline constexpr uint32_t kPointShadowFaceCount = 6;

// Push-constant indices the shadow depth pass uses to select its
// light matrix (shadow.vert): 0..kMaxShadowCascades-1 are the
// directional cascades, kSpotShadowPushIndex the spot, and
// kPointShadowPushBase + face the point cube faces.
inline constexpr uint32_t kSpotShadowPushIndex = 4;
inline constexpr uint32_t kPointShadowPushBase = 5;

// Soft-shadow filter radius, in shadow-map texels, shared by the
// cascade / spot / point PCF kernels (12-tap Poisson disk). Larger
// reads softer (and pricier); 1.6 texels hides the 1024/2048px
// texel grid without washing contact shadows out.
inline constexpr float kShadowPcfRadiusTexels = 1.6f;

// World axis each cube face looks along (face order is the classic
// +X, -X, +Y, -Y, +Z, -Z). Mirrored by cube.frag's face selection.
[[nodiscard]] ENGINE_GRAPHICS_API glm::vec3 pointShadowFaceDirection(uint32_t face);
// Which cube face owns a fragment-to-light direction (dominant
// axis). Mirrored by cube.frag's face selection.
[[nodiscard]] ENGINE_GRAPHICS_API uint32_t pointShadowFaceIndex(const glm::vec3& direction);
// One face's light-space view-projection (90-degree perspective).
[[nodiscard]] ENGINE_GRAPHICS_API glm::mat4 pointShadowViewProjection(const glm::vec3& position, uint32_t face,
                                                                      float nearPlane, float farPlane);
// All six face matrices for a point light at `position`.
[[nodiscard]] ENGINE_GRAPHICS_API std::array<glm::mat4, kPointShadowFaceCount> pointShadowViewProjections(
    const glm::vec3& position, float nearPlane, float farPlane);

// Fits one light view-projection per cascade around its frustum
// slice (bounding-sphere fit, texel-snapped against shimmer).
// cameraUp need not be orthogonal to cameraFront.
[[nodiscard]] ENGINE_GRAPHICS_API std::array<ShadowCascade, kMaxShadowCascades> computeShadowCascades(
    const glm::vec3& cameraPosition, const glm::vec3& cameraFront, const glm::vec3& cameraUp,
    float fieldOfViewYRadians, float aspectRatio, float nearPlane, float farPlane,
    const glm::vec3& lightDirection, uint32_t cascadeCount, float splitLambda, uint32_t shadowMapSize);

} // namespace engine
