#pragma once

#include "Core/Export.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Environment lighting data (Phase 22): procedural sky, diffuse
// irradiance convolution, GGX-prefiltered specular reflections and
// the split-sum BRDF integration LUT.
//
// Everything is generated deterministically on the CPU from
// quasi-random (Hammersley) sample sequences — no RNG state, so the
// same inputs always produce bit-identical maps and the whole
// pipeline is unit-testable without a GPU. The Renderer uploads the
// results as Vulkan cubemaps / a 2D LUT; shaders evaluate image-based
// lighting with the classic split-sum approximation:
//
//   specularIBL ≈ prefiltered(R, roughness) * (F * LUT.x + LUT.y)
//   diffuseIBL  ≈ irradiance(N) * albedo          (irradiance / pi)
//
// Cube faces follow the Vulkan/OpenGL convention: 0:+X 1:-X 2:+Y
// 3:-Y 4:+Z 5:-Z, texel (0,0) at the top-left of each face.
// ---------------------------------------------------------------------------

struct CubeMapData {
    uint32_t size = 0; // texels per face edge
    std::array<std::vector<glm::vec3>, 6> faces; // HDR linear RGB, size*size each

    [[nodiscard]] const glm::vec3& texel(uint32_t face, uint32_t x, uint32_t y) const {
        return faces[face][static_cast<size_t>(y) * size + x];
    }
};

// Unit direction for a face UV (u, v in [0, 1], v = 0 at the top),
// and the exact inverse mapping (major-axis selection).
[[nodiscard]] ENGINE_GRAPHICS_API glm::vec3 cubeFaceDirection(uint32_t face, float u, float v);
void ENGINE_GRAPHICS_API directionToCubeFace(const glm::vec3& direction, uint32_t& outFace, float& outU, float& outV);

// Bilinear sample with edge clamping (slight cross-face bleed is
// acceptable for blurred convolution results).
[[nodiscard]] ENGINE_GRAPHICS_API glm::vec3 sampleCubeBilinear(const CubeMapData& map, const glm::vec3& direction);

// Procedural sky radiance: zenith/horizon gradient, warm sun disc +
// halo aligned with the key directional light, dim ground bounce
// below the horizon. HDR (sun peaks well above 1.0 so bloom bites).
[[nodiscard]] ENGINE_GRAPHICS_API glm::vec3 skyRadiance(const glm::vec3& direction, const glm::vec3& sunDirection);

[[nodiscard]] ENGINE_GRAPHICS_API CubeMapData generateEnvironmentCube(uint32_t size, const glm::vec3& sunDirection);

// Cosine-weighted irradiance convolution. Stored value is E / pi,
// so a constant environment reproduces itself exactly.
[[nodiscard]] ENGINE_GRAPHICS_API CubeMapData generateIrradiance(const CubeMapData& environment, uint32_t size,
                                                                 uint32_t sampleCount = 512);

// One cube per roughness level: level 0 is the sharp reflection,
// deeper levels blurrier; sizes halve per level (min 1).
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<CubeMapData> generatePrefiltered(
    const CubeMapData& environment, uint32_t baseSize, uint32_t levels, uint32_t sampleCount = 256);

// Split-sum BRDF LUT: texel (x, y) = (scale, bias) for
// NoV = (x + 0.5) / size, roughness = (y + 0.5) / size, indexed
// [y * size + x]; the shader samples it at vec2(NoV, roughness).
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<glm::vec2> integrateBrdfLut(uint32_t size,
                                                                            uint32_t sampleCount = 1024);

} // namespace engine
