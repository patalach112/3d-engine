#include "Graphics/ShadowMap.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace engine {

glm::mat4 directionalLightViewProjection(const glm::vec3& lightDirection, const glm::vec3& center,
                                         float extent, float lightDistance, float nearPlane, float farPlane) {
    const glm::vec3 direction = glm::normalize(lightDirection);
    const glm::vec3 lightPosition = center + direction * lightDistance;
    // Any up vector not parallel to the light direction; the key light is
    // steep (y-dominant), so world X is a safe reference.
    const glm::vec3 up = std::abs(direction.x) < 0.9f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::vec3{0.0f, 0.0f, 1.0f};
    const glm::mat4 lightView = glm::lookAt(lightPosition, center, up);
    const glm::mat4 lightProjection = glm::ortho(-extent, extent, -extent, extent, nearPlane, farPlane);
    return lightProjection * lightView;
}

glm::mat4 spotShadowViewProjection(const glm::vec3& position, const glm::vec3& direction,
                                   float outerConeRadians, float nearPlane, float farPlane) {
    const glm::vec3 dir = glm::normalize(direction);
    // Any up vector not parallel to the spot direction; spots in
    // the editor usually aim downward, so world Y is not safe.
    const glm::vec3 up = std::abs(dir.y) < 0.99f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
    const glm::mat4 view = glm::lookAt(position, position + dir, up);
    // The cone half-angle becomes the perspective half-FOV;
    // clamped so a degenerate cone cannot produce a singular or
    // backward projection.
    const float halfFov = std::clamp(outerConeRadians, 0.01f, 1.55f);
    glm::mat4 projection = glm::perspective(2.0f * halfFov, 1.0f, nearPlane, farPlane);
    projection[1][1] *= -1.0f; // Vulkan clip space (matches Camera)
    return projection * view;
}

std::array<float, kMaxShadowCascades> computeCascadeSplits(float nearPlane, float farPlane,
                                                           uint32_t cascadeCount, float splitLambda) {
    std::array<float, kMaxShadowCascades> splits{};
    splits.fill(farPlane);
    const uint32_t count = std::max(1u, std::min(cascadeCount, kMaxShadowCascades));
    const float lambda = glm::clamp(splitLambda, 0.0f, 1.0f);
    const float safeNear = std::max(nearPlane, 0.001f);
    for (uint32_t i = 0; i < count; ++i) {
        const float fraction = static_cast<float>(i + 1) / static_cast<float>(count);
        const float logarithmic = safeNear * std::pow(farPlane / safeNear, fraction);
        const float uniform = safeNear + (farPlane - safeNear) * fraction;
        splits[i] = lambda * logarithmic + (1.0f - lambda) * uniform;
    }
    splits[count - 1] = farPlane;
    return splits;
}

std::array<ShadowCascade, kMaxShadowCascades> computeShadowCascades(
    const glm::vec3& cameraPosition, const glm::vec3& cameraFront, const glm::vec3& cameraUp,
    float fieldOfViewYRadians, float aspectRatio, float nearPlane, float farPlane,
    const glm::vec3& lightDirection, uint32_t cascadeCount, float splitLambda, uint32_t shadowMapSize) {
    std::array<ShadowCascade, kMaxShadowCascades> cascades{};
    const uint32_t count = std::max(1u, std::min(cascadeCount, kMaxShadowCascades));
    const std::array<float, kMaxShadowCascades> splits =
        computeCascadeSplits(nearPlane, farPlane, count, splitLambda);

    const glm::vec3 front = glm::normalize(cameraFront);
    const glm::vec3 right = glm::normalize(glm::cross(front, cameraUp));
    const glm::vec3 up = glm::cross(right, front);
    const glm::vec3 lightDir = glm::normalize(lightDirection);
    const glm::vec3 lightUp = std::abs(lightDir.x) < 0.9f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::vec3{0.0f, 0.0f, 1.0f};
    const float tanHalfFovY = std::tan(fieldOfViewYRadians * 0.5f);

    float sliceNear = nearPlane;
    for (uint32_t cascadeIndex = 0; cascadeIndex < count; ++cascadeIndex) {
        const float sliceFar = splits[cascadeIndex];

        std::array<glm::vec3, 8> corners{};
        glm::vec3 center{0.0f};
        size_t cornerIndex = 0;
        for (const float depth : {sliceNear, sliceFar}) {
            const float halfHeight = depth * tanHalfFovY;
            const float halfWidth = halfHeight * aspectRatio;
            const glm::vec3 sliceCenter = cameraPosition + front * depth;
            for (const float sx : {-1.0f, 1.0f}) {
                for (const float sy : {-1.0f, 1.0f}) {
                    corners[cornerIndex++] = sliceCenter + right * (sx * halfWidth) + up * (sy * halfHeight);
                }
            }
        }
        for (const glm::vec3& corner : corners) {
            center += corner;
        }
        center /= static_cast<float>(corners.size());
        float radius = 0.0f;
        for (const glm::vec3& corner : corners) {
            radius = std::max(radius, glm::length(corner - center));
        }
        radius = std::max(radius, 0.01f);

        const float lightDistance = radius * 2.0f;
        const glm::vec3 lightPosition = center + lightDir * lightDistance;
        glm::mat4 lightProjection = glm::ortho(-radius, radius, -radius, radius, 0.1f, lightDistance + radius * 2.0f);
        const glm::mat4 lightView = glm::lookAt(lightPosition, center, lightUp);

        // Texel snapping: shift the projection so the slice center
        // lands on a whole texel of light NDC space.
        if (shadowMapSize > 0) {
            const glm::vec4 projectedCenter = lightProjection * lightView * glm::vec4{center, 1.0f};
            const float texelSize = 2.0f / static_cast<float>(shadowMapSize);
            const float snappedX = std::floor(projectedCenter.x / texelSize) * texelSize;
            const float snappedY = std::floor(projectedCenter.y / texelSize) * texelSize;
            lightProjection[3][0] += snappedX - projectedCenter.x;
            lightProjection[3][1] += snappedY - projectedCenter.y;
        }

        cascades[cascadeIndex].lightViewProj = lightProjection * lightView;
        cascades[cascadeIndex].splitFarDepth = sliceFar;
        sliceNear = sliceFar;
    }
    for (uint32_t cascadeIndex = count; cascadeIndex < kMaxShadowCascades; ++cascadeIndex) {
        cascades[cascadeIndex] = cascades[count - 1];
    }
    return cascades;
}

} // namespace engine
