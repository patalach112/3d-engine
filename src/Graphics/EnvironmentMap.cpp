#include "Graphics/EnvironmentMap.h"

#include "Graphics/Pbr.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <cmath>

namespace engine {
namespace {

// Van der Corput radical inverse — the deterministic low-discrepancy
// core of every sample sequence below.
float radicalInverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f; // 2^-32
}

glm::vec2 hammersley(uint32_t index, uint32_t count) {
    return glm::vec2{static_cast<float>(index) / static_cast<float>(count), radicalInverse(index)};
}

// Builds an orthonormal tangent frame around a unit normal.
void tangentFrame(const glm::vec3& normal, glm::vec3& outTangent, glm::vec3& outBitangent) {
    const glm::vec3 reference = std::abs(normal.y) < 0.99f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
    outTangent = glm::normalize(glm::cross(reference, normal));
    outBitangent = glm::cross(normal, outTangent);
}

// GGX importance sampling: a half-vector in world space for the given
// roughness (same distribution the shader uses).
glm::vec3 importanceSampleGGX(const glm::vec2& xi, const glm::vec3& normal, float roughness) {
    const float alpha = roughness * roughness;
    const float phi = 2.0f * kPi * xi.x;
    const float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    const float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
    const glm::vec3 tangentSpace{sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
    glm::vec3 tangent, bitangent;
    tangentFrame(normal, tangent, bitangent);
    return glm::normalize(tangent * tangentSpace.x + bitangent * tangentSpace.y + normal * tangentSpace.z);
}

} // namespace

glm::vec3 cubeFaceDirection(uint32_t face, float u, float v) {
    const float s = 2.0f * u - 1.0f;
    const float t = 2.0f * v - 1.0f;
    glm::vec3 direction{0.0f};
    switch (face) {
        case 0: direction = glm::vec3{1.0f, -t, -s}; break;  // +X
        case 1: direction = glm::vec3{-1.0f, -t, s}; break;  // -X
        case 2: direction = glm::vec3{s, 1.0f, t}; break;    // +Y
        case 3: direction = glm::vec3{s, -1.0f, -t}; break;  // -Y
        case 4: direction = glm::vec3{s, -t, 1.0f}; break;   // +Z
        default: direction = glm::vec3{-s, -t, -1.0f}; break; // -Z
    }
    return glm::normalize(direction);
}

void directionToCubeFace(const glm::vec3& direction, uint32_t& outFace, float& outU, float& outV) {
    const glm::vec3 absDir = glm::abs(direction);
    float s = 0.0f;
    float t = 0.0f;
    float major = 1.0f;
    if (absDir.x >= absDir.y && absDir.x >= absDir.z) {
        major = absDir.x;
        if (direction.x > 0.0f) {
            outFace = 0; s = -direction.z; t = -direction.y;
        } else {
            outFace = 1; s = direction.z; t = -direction.y;
        }
    } else if (absDir.y >= absDir.x && absDir.y >= absDir.z) {
        major = absDir.y;
        if (direction.y > 0.0f) {
            outFace = 2; s = direction.x; t = direction.z;
        } else {
            outFace = 3; s = direction.x; t = -direction.z;
        }
    } else {
        major = absDir.z;
        if (direction.z > 0.0f) {
            outFace = 4; s = direction.x; t = -direction.y;
        } else {
            outFace = 5; s = -direction.x; t = -direction.y;
        }
    }
    outU = 0.5f * (s / major + 1.0f);
    outV = 0.5f * (t / major + 1.0f);
}

glm::vec3 sampleCubeBilinear(const CubeMapData& map, const glm::vec3& direction) {
    if (map.size == 0) {
        return glm::vec3{0.0f};
    }
    uint32_t face = 0;
    float u = 0.0f;
    float v = 0.0f;
    directionToCubeFace(direction, face, u, v);
    const float maxCoord = static_cast<float>(map.size - 1);
    const float fx = glm::clamp(u * static_cast<float>(map.size) - 0.5f, 0.0f, maxCoord);
    const float fy = glm::clamp(v * static_cast<float>(map.size) - 0.5f, 0.0f, maxCoord);
    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = glm::min(x0 + 1, map.size - 1);
    const uint32_t y1 = glm::min(y0 + 1, map.size - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const glm::vec3 top = glm::mix(map.texel(face, x0, y0), map.texel(face, x1, y0), tx);
    const glm::vec3 bottom = glm::mix(map.texel(face, x0, y1), map.texel(face, x1, y1), tx);
    return glm::mix(top, bottom, ty);
}

glm::vec3 skyRadiance(const glm::vec3& direction, const glm::vec3& sunDirection) {
    const glm::vec3 zenith{0.22f, 0.42f, 0.78f};
    const glm::vec3 horizon{0.74f, 0.80f, 0.88f};
    const glm::vec3 ground{0.16f, 0.13f, 0.11f};
    const glm::vec3 sunColor{1.0f, 0.88f, 0.70f};

    const float heightMix = std::pow(glm::clamp(direction.y, 0.0f, 1.0f), 0.55f);
    glm::vec3 sky = glm::mix(horizon, zenith, heightMix);
    if (direction.y < 0.0f) {
        sky = glm::mix(horizon, ground, glm::clamp(-direction.y * 3.0f, 0.0f, 1.0f));
    }
    const float sunAmount = glm::max(glm::dot(direction, glm::normalize(sunDirection)), 0.0f);
    sky += sunColor * std::pow(sunAmount, 350.0f) * 12.0f; // disc: HDR peak feeds bloom
    sky += sunColor * std::pow(sunAmount, 8.0f) * 0.28f;   // warm halo
    return sky;
}

CubeMapData generateEnvironmentCube(uint32_t size, const glm::vec3& sunDirection) {
    CubeMapData map;
    if (size == 0) {
        return map;
    }
    map.size = size;
    const glm::vec3 sun = glm::normalize(sunDirection);
    for (uint32_t face = 0; face < 6; ++face) {
        map.faces[face].resize(static_cast<size_t>(size) * size);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                map.faces[face][static_cast<size_t>(y) * size + x] =
                    skyRadiance(cubeFaceDirection(face, u, v), sun);
            }
        }
    }
    return map;
}

CubeMapData generateIrradiance(const CubeMapData& environment, uint32_t size, uint32_t sampleCount) {
    CubeMapData map;
    if (size == 0 || environment.size == 0 || sampleCount == 0) {
        return map;
    }
    map.size = size;
    for (uint32_t face = 0; face < 6; ++face) {
        map.faces[face].resize(static_cast<size_t>(size) * size);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                const glm::vec3 normal = cubeFaceDirection(face, u, v);
                glm::vec3 tangent, bitangent;
                tangentFrame(normal, tangent, bitangent);
                glm::vec3 sum{0.0f};
                for (uint32_t i = 0; i < sampleCount; ++i) {
                    // Cosine-weighted hemisphere sampling: the
                    // estimator of E/pi is the plain mean of L.
                    const glm::vec2 xi = hammersley(i, sampleCount);
                    const float phi = 2.0f * kPi * xi.x;
                    const float cosTheta = std::sqrt(1.0f - xi.y);
                    const float sinTheta = std::sqrt(xi.y);
                    const glm::vec3 sampleDir = glm::normalize(tangent * (sinTheta * std::cos(phi)) +
                                                               bitangent * (sinTheta * std::sin(phi)) +
                                                               normal * cosTheta);
                    sum += sampleCubeBilinear(environment, sampleDir);
                }
                map.faces[face][static_cast<size_t>(y) * size + x] = sum / static_cast<float>(sampleCount);
            }
        }
    }
    return map;
}

std::vector<CubeMapData> generatePrefiltered(const CubeMapData& environment, uint32_t baseSize, uint32_t levels,
                                             uint32_t sampleCount) {
    std::vector<CubeMapData> mips;
    if (baseSize == 0 || environment.size == 0 || levels == 0 || sampleCount == 0) {
        return mips;
    }
    mips.reserve(levels);
    for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t size = glm::max(1u, baseSize >> level);
        const float roughness = levels > 1 ? static_cast<float>(level) / static_cast<float>(levels - 1) : 0.0f;
        CubeMapData map;
        map.size = size;
        for (uint32_t face = 0; face < 6; ++face) {
            map.faces[face].resize(static_cast<size_t>(size) * size);
            for (uint32_t y = 0; y < size; ++y) {
                for (uint32_t x = 0; x < size; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                    const glm::vec3 reflection = cubeFaceDirection(face, u, v); // N = V = R
                    glm::vec3 result;
                    if (roughness <= 0.001f) {
                        result = sampleCubeBilinear(environment, reflection);
                    } else {
                        glm::vec3 sum{0.0f};
                        float weightSum = 0.0f;
                        for (uint32_t i = 0; i < sampleCount; ++i) {
                            const glm::vec3 halfVec = importanceSampleGGX(hammersley(i, sampleCount), reflection,
                                                                          roughness);
                            const glm::vec3 lightDir =
                                glm::normalize(2.0f * glm::dot(reflection, halfVec) * halfVec - reflection);
                            const float noL = glm::dot(reflection, lightDir);
                            if (noL > 0.0f) {
                                sum += sampleCubeBilinear(environment, lightDir) * noL;
                                weightSum += noL;
                            }
                        }
                        result = weightSum > 0.0f ? sum / weightSum : glm::vec3{0.0f};
                    }
                    map.faces[face][static_cast<size_t>(y) * size + x] = result;
                }
            }
        }
        mips.push_back(std::move(map));
    }
    return mips;
}

std::vector<glm::vec2> integrateBrdfLut(uint32_t size, uint32_t sampleCount) {
    std::vector<glm::vec2> lut;
    if (size == 0 || sampleCount == 0) {
        return lut;
    }
    lut.resize(static_cast<size_t>(size) * size);
    for (uint32_t y = 0; y < size; ++y) {
        const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (uint32_t x = 0; x < size; ++x) {
            const float noV = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const glm::vec3 viewDir{std::sqrt(1.0f - noV * noV), 0.0f, noV}; // tangent-space view
            const glm::vec3 normal{0.0f, 0.0f, 1.0f};
            float scale = 0.0f;
            float bias = 0.0f;
            for (uint32_t i = 0; i < sampleCount; ++i) {
                const glm::vec3 halfVec = importanceSampleGGX(hammersley(i, sampleCount), normal, roughness);
                const glm::vec3 lightDir = glm::normalize(2.0f * glm::dot(viewDir, halfVec) * halfVec - viewDir);
                const float noL = glm::clamp(lightDir.z, 0.0f, 1.0f);
                const float noH = glm::clamp(halfVec.z, 0.0f, 1.0f);
                const float voH = glm::clamp(glm::dot(viewDir, halfVec), 0.0f, 1.0f);
                if (noL > 0.0f) {
                    const float geometry =
                        geometrySmith(noV, noL, roughness) * voH / glm::max(noH * noV, 1e-5f);
                    const float fresnelComplement = std::pow(1.0f - voH, 5.0f);
                    scale += (1.0f - fresnelComplement) * geometry;
                    bias += fresnelComplement * geometry;
                }
            }
            lut[static_cast<size_t>(y) * size + x] =
                glm::vec2{scale / static_cast<float>(sampleCount), bias / static_cast<float>(sampleCount)};
        }
    }
    return lut;
}

} // namespace engine
