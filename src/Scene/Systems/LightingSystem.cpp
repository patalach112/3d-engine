#include "Scene/Systems/LightingSystem.h"

#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Systems/TransformSystem.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine {

SceneLightData LightingSystem::gather(const Scene& scene) { return gather(scene, 16.0f / 9.0f, kShadowMapSize); }

SceneLightData LightingSystem::gather(const Scene& scene, float aspectRatio, uint32_t shadowMapSize) {
    SceneLightData data;
    data.cascadeMatrices.fill(glm::mat4{1.0f});

    // --- Directional key light: an enabled DirectionalLightComponent
    // drives the sun (highest intensity wins); with none, the legacy
    // fixed sun keeps camera-less/older scenes lit identically.
    data.directionalDirection = glm::normalize(directionalLightDirection());
    data.directionalRadiance = 1.0f;
    uint32_t cascadeCount = kMaxShadowCascades;
    {
        const auto sunView = scene.registry().view<DirectionalLightComponent>();
        const DirectionalLightComponent* sun = nullptr;
        for (const entt::entity entity : sunView) {
            const DirectionalLightComponent& light = sunView.get<DirectionalLightComponent>(entity);
            if (light.enabled && (sun == nullptr || light.intensity > sun->intensity)) {
                sun = &light;
            }
        }
        if (sun != nullptr) {
            data.directionalFromEntity = true;
            if (glm::dot(sun->direction, sun->direction) > 1e-8f) {
                data.directionalDirection = glm::normalize(sun->direction);
            }
            data.directionalColor = sun->color;
            data.directionalRadiance = sun->intensity;
            data.directionalCastShadow = sun->castShadow;
            cascadeCount = std::clamp(sun->cascadeCount, 1u, kMaxShadowCascades);
        }
    }

    const CameraComponent* activeCamera = scene.findActiveCamera();
    glm::vec3 referencePoint{0.0f};
    if (activeCamera != nullptr) {
        const Camera& camera = activeCamera->camera;
        const CameraConfig& config = camera.config();
        const std::array<ShadowCascade, kMaxShadowCascades> cascades = computeShadowCascades(
            camera.position(), camera.front(), camera.up(), glm::radians(config.fieldOfViewDegrees),
            aspectRatio, config.nearPlane, config.farPlane, data.directionalDirection, cascadeCount,
            0.7f, shadowMapSize);
        data.cascadeCount = cascadeCount;
        data.cascadesFilled = true;
        for (uint32_t i = 0; i < kMaxShadowCascades; ++i) {
            // Cascades past the count repeat the last slice (and
            // its split), so the shader's depth pick can never
            // address an unfilled matrix.
            const uint32_t slice = std::min(i, cascadeCount - 1);
            data.cascadeMatrices[i] = cascades[slice].lightViewProj;
            data.cascadeSplits[static_cast<int>(i)] = cascades[slice].splitFarDepth;
        }
        referencePoint = camera.position();
    } else {
        // Legacy single-cascade fallback (pre-Phase-21 behaviour).
        data.cascadeCount = 1;
        data.cascadesFilled = true;
        data.cascadeMatrices[0] =
            directionalLightViewProjection(data.directionalDirection, glm::vec3{0.0f}, 30.0f, 60.0f, 0.5f, 130.0f);
        data.cascadeSplits = glm::vec4{60.0f, 60.0f, 60.0f, 60.0f};
    }

    // --- Dynamic point / spot lights, nearest-first to the camera.
    struct Candidate {
        float distanceSquared = 0.0f;
        glm::vec4 positionRange{};
        glm::vec4 colorIntensity{};
        glm::vec4 directionType{};
        glm::vec4 cone{};
        bool isSpot = false;
        bool castShadow = false;       // spots only: wants the shadow map
        float outerConeRadians = 0.0f; // spots only: shadow projection FOV
    };

    std::vector<Candidate> candidates;
    const auto collect = [&candidates, &referencePoint](const glm::vec3& position, const glm::vec3& direction,
                                                        const glm::vec3& color, float intensity, float range,
                                                        float innerCone, float outerCone, bool isSpot,
                                                        bool castShadow) {
        Candidate candidate;
        const glm::vec3 delta = position - referencePoint;
        candidate.distanceSquared = glm::dot(delta, delta);
        candidate.positionRange = glm::vec4{position, range};
        candidate.colorIntensity = glm::vec4{color, intensity};
        candidate.directionType = glm::vec4{direction, isSpot ? 1.0f : 0.0f};
        candidate.cone = glm::vec4{std::cos(innerCone), std::cos(outerCone), 0.0f, 0.0f};
        candidate.isSpot = isSpot;
        candidate.castShadow = castShadow;
        candidate.outerConeRadians = outerCone;
        candidates.push_back(candidate);
    };

    const auto pointView = scene.registry().view<TransformComponent, PointLightComponent>();
    for (const entt::entity entity : pointView) {
        const PointLightComponent& light = pointView.get<PointLightComponent>(entity);
        if (!light.enabled || light.intensity <= 0.0f) {
            continue;
        }
        const TransformComponent& transform = pointView.get<TransformComponent>(entity);
        const WorldTransformComponent* world = scene.registry().try_get<WorldTransformComponent>(entity);
        const glm::mat4 matrix = TransformSystem::effectiveWorldMatrix(transform, world);
        collect(glm::vec3{matrix[3]}, glm::vec3{0.0f}, light.color, light.intensity, light.range, 0.0f, 0.0f,
                false, false);
    }
    const auto spotView = scene.registry().view<TransformComponent, SpotLightComponent>();
    for (const entt::entity entity : spotView) {
        const SpotLightComponent& light = spotView.get<SpotLightComponent>(entity);
        if (!light.enabled || light.intensity <= 0.0f) {
            continue;
        }
        const TransformComponent& transform = spotView.get<TransformComponent>(entity);
        const WorldTransformComponent* world = scene.registry().try_get<WorldTransformComponent>(entity);
        const glm::mat4 matrix = TransformSystem::effectiveWorldMatrix(transform, world);
        glm::vec3 position{0.0f};
        glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
        if (world != nullptr) {
            position = world->worldPosition;
            orientation = world->worldOrientation;
        } else {
            TransformSystem::decompose(matrix, position, orientation, scale);
        }
        const glm::vec3 direction = glm::normalize(orientation * glm::vec3{0.0f, 0.0f, -1.0f});
        (void)scale;
        collect(position, direction, light.color, light.intensity, light.range, light.innerConeRadians,
                light.outerConeRadians, true, light.castShadow);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.distanceSquared < b.distanceSquared; });
    const size_t selected = std::min(candidates.size(), static_cast<size_t>(kMaxSceneLights));
    uint32_t pointCount = 0;
    uint32_t spotCount = 0;
    for (size_t i = 0; i < selected; ++i) {
        const Candidate& candidate = candidates[i];
        data.lightPositionRange[i] = candidate.positionRange;
        data.lightColorIntensity[i] = candidate.colorIntensity;
        data.lightDirectionType[i] = candidate.directionType;
        data.lightCone[i] = candidate.cone;
        if (candidate.isSpot) {
            ++spotCount;
        } else {
            ++pointCount;
        }
    }
    data.lightCounts = glm::vec4{static_cast<float>(pointCount), static_cast<float>(spotCount),
                                 static_cast<float>(selected), 0.0f};

    // Phase 32: one spot shadow map is budgeted — the nearest
    // packed spot that asked for shadows owns it. A shadow spot
    // outside the 16-light budget casts none this frame.
    for (size_t i = 0; i < selected; ++i) {
        const Candidate& candidate = candidates[i];
        if (candidate.isSpot && candidate.castShadow) {
            const glm::vec3 position = glm::vec3{candidate.positionRange};
            const glm::vec3 direction = glm::vec3{candidate.directionType};
            const float range = std::max(candidate.positionRange.w, 0.1f);
            data.spotShadowEnabled = true;
            data.spotShadowLightIndex = static_cast<int32_t>(i);
            data.spotShadowViewProj = spotShadowViewProjection(position, direction, candidate.outerConeRadians,
                                                               std::max(0.05f, range * 0.01f), range);
            break;
        }
    }
    return data;
}

} // namespace engine
