#pragma once

#include "Graphics/ShadowMap.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace engine {

class Scene;

// ---------------------------------------------------------------------------
// Scene light data (Phase 21)
//
// Everything the renderer needs for one frame of dynamic lighting,
// gathered from Point/SpotLightComponents on the CPU and uploaded in
// the per-frame uniform buffer:
//   * up to kMaxShadowCascades cascaded shadow slices for the key
//     directional light (see ShadowMap.h), with matching split depths;
//   * up to kMaxSceneLights nearest dynamic lights packed into vec4
//     arrays for the fragment shader.
//
// Packed light record layout (kept byte-identical with the GLSL):
//   positionRange.xyz = world position, .w = range
//   colorIntensity.rgb = color, .w = intensity
//   directionType.xyz = spot direction (zero for points), .w = type (0 point, 1 spot)
//   coneAngles.x = cos(inner), .y = cos(outer), .zw unused
// ---------------------------------------------------------------------------
inline constexpr uint32_t kMaxSceneLights = 16;

struct SceneLightData {
    uint32_t cascadeCount = 1;
    bool cascadesFilled = false; // true once gather() has computed matrices
    // View-space far depth per cascade; padded entries repeat the last.
    glm::vec4 cascadeSplits{60.0f, 60.0f, 60.0f, 60.0f};
    std::array<glm::mat4, kMaxShadowCascades> cascadeMatrices{};
    // .x = pointCount, .y = spotCount, .z = totalCount
    glm::vec4 lightCounts{0.0f};
    std::array<glm::vec4, kMaxSceneLights> lightPositionRange{};
    std::array<glm::vec4, kMaxSceneLights> lightColorIntensity{};
    std::array<glm::vec4, kMaxSceneLights> lightDirectionType{};
    std::array<glm::vec4, kMaxSceneLights> lightCone{};
    glm::vec3 directionalDirection{0.35f, 0.8f, 0.45f};
    float directionalRadiance = 1.0f;
};

// Smooth windowed inverse-square attenuation, mirrored exactly by
// the fragment shader: full strength at the light, fading to zero
// at range. CPU mirror exists for tests and tooling.
[[nodiscard]] inline float lightAttenuation(float distance, float range) {
    if (range <= 0.0f) {
        return 0.0f;
    }
    const float ratio = glm::clamp(distance / range, 0.0f, 1.0f);
    const float window = glm::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
    const float windowed = window * window;
    return windowed / (distance * distance + 1.0f);
}

// Spot cone falloff: 1 inside the inner cone, 0 outside the outer.
[[nodiscard]] inline float spotConeFactor(float cosTheta, float innerConeRadians, float outerConeRadians) {
    const float cosInner = std::cos(innerConeRadians);
    const float cosOuter = std::cos(outerConeRadians);
    if (cosInner <= cosOuter) {
        return cosTheta >= cosInner ? 1.0f : 0.0f;
    }
    return glm::clamp((cosTheta - cosOuter) / (cosInner - cosOuter), 0.0f, 1.0f);
}

class ENGINE_SCENE_API LightingSystem {
public:
    // Gathers the frame's lighting. When the scene has an active
    // camera, cascades are fitted to it and dynamic lights are
    // selected nearest-first to the camera, so the 16-slot budget
    // always lights what the player is looking at.
    // aspectRatio feeds the camera's projection for cascade fitting
    // (prepareFrame knows it; the scene-only overload assumes 16:9).
    [[nodiscard]] static SceneLightData gather(const Scene& scene, float aspectRatio,
                                               uint32_t shadowMapSize = kShadowMapSize);
    [[nodiscard]] static SceneLightData gather(const Scene& scene);
};

} // namespace engine
