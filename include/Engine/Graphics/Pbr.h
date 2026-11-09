#pragma once

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>

namespace engine {

// ---------------------------------------------------------------------------
// Cook-Torrance microfacet BRDF (Phase 22) — CPU mirror of cube.frag.
//
// The fragment shader evaluates exactly these formulas (GGX /
// Trowbridge-Reitz normal distribution, Smith geometry with the
// Schlick-GGX approximation, Schlick Fresnel), so the functions here
// are the testable ground truth for the GPU path: same constants,
// same clamps. All inputs are unit vectors unless noted; roughness
// and metallic are in [0, 1].
// ---------------------------------------------------------------------------
inline constexpr float kPi = 3.14159265358979323846f;

// GGX/Trowbridge-Reitz normal distribution: probability density of
// microfacet normals aligned with the half-vector. alpha = roughness^2
// (the UE4 reparameterization: perceptually linear roughness).
[[nodiscard]] inline float distributionGGX(float noH, float roughness) {
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    const float denom = noH * noH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / (kPi * denom * denom);
}

// Schlick-GGX approximation of the Smith geometry term for one
// direction (direct lighting remap k = (roughness+1)^2 / 8).
[[nodiscard]] inline float geometrySchlickGGX(float noX, float roughness) {
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return noX / (noX * (1.0f - k) + k);
}

// Smith geometry: masking (view) combined with shadowing (light).
[[nodiscard]] inline float geometrySmith(float noV, float noL, float roughness) {
    return geometrySchlickGGX(noV, roughness) * geometrySchlickGGX(noL, roughness);
}

// Schlick Fresnel approximation: F0 at normal incidence rising to 1
// at grazing angles.
[[nodiscard]] inline glm::vec3 fresnelSchlick(float cosTheta, const glm::vec3& f0) {
    const float m = glm::clamp(1.0f - cosTheta, 0.0f, 1.0f);
    const float m2 = m * m;
    return f0 + (glm::vec3{1.0f} - f0) * (m2 * m2 * m);
}

// Fresnel with a roughness-aware ceiling, used for image-based
// specular so rough surfaces do not reflect like mirrors at grazing
// angles (the standard split-sum companion term).
[[nodiscard]] inline glm::vec3 fresnelSchlickRoughness(float cosTheta, const glm::vec3& f0, float roughness) {
    const float m = glm::clamp(1.0f - cosTheta, 0.0f, 1.0f);
    const float m2 = m * m;
    return f0 + (glm::max(glm::vec3{1.0f - roughness}, f0) - f0) * (m2 * m2 * m);
}

// Dielectric base reflectance (4% — plastic/glass territory).
inline constexpr glm::vec3 kDielectricF0{0.04f};

// Full direct-light evaluation for one light: returns outgoing
// radiance (already multiplied by the light's radiance and NoL).
// `radiance` is the light color * intensity * attenuation at this
// fragment. Metals tint their reflections with the albedo and have
// no diffuse term; dielectrics keep an energy-conserving diffuse.
[[nodiscard]] inline glm::vec3 evaluateDirectLight(const glm::vec3& normal, const glm::vec3& viewDir,
                                                   const glm::vec3& lightDir, const glm::vec3& radiance,
                                                   const glm::vec3& albedo, float metallic, float roughness) {
    const glm::vec3 halfVec = glm::normalize(lightDir + viewDir);
    const float noL = glm::clamp(glm::dot(normal, lightDir), 0.0f, 1.0f);
    const float noV = glm::clamp(glm::dot(normal, viewDir), 0.0f, 1.0f);
    const float noH = glm::clamp(glm::dot(normal, halfVec), 0.0f, 1.0f);
    const float voH = glm::clamp(glm::dot(viewDir, halfVec), 0.0f, 1.0f);
    if (noL <= 0.0f || noV <= 0.0f) {
        return glm::vec3{0.0f};
    }

    const glm::vec3 f0 = glm::mix(kDielectricF0, albedo, metallic);
    const glm::vec3 fresnel = fresnelSchlick(voH, f0);
    const float distribution = distributionGGX(noH, roughness);
    const float geometry = geometrySmith(noV, noL, roughness);

    const glm::vec3 specular = (distribution * geometry) * fresnel / (4.0f * noV * noL);
    const glm::vec3 diffuseWeight = (glm::vec3{1.0f} - fresnel) * (1.0f - metallic);
    return (diffuseWeight * albedo / kPi + specular) * radiance * noL;
}

} // namespace engine
