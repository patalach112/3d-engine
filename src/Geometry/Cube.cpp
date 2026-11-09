#include "Geometry/Cube.h"

namespace engine {

std::vector<Vertex> Cube::vertices() {
    constexpr float h = 0.5f; // half extent

    // One neutral white tint for every face: per-object color lives in
    // the material (MaterialDesc::baseColor, chosen per scene category),
    // and per-face variation comes only from normal-based lighting in the
    // shader, never from debug face colors.
    constexpr glm::vec3 kMaterialTint{1.0f, 1.0f, 1.0f};

    constexpr glm::vec3 kFront{0.0f, 0.0f, 1.0f};
    constexpr glm::vec3 kBack{0.0f, 0.0f, -1.0f};
    constexpr glm::vec3 kRight{1.0f, 0.0f, 0.0f};
    constexpr glm::vec3 kLeft{-1.0f, 0.0f, 0.0f};
    constexpr glm::vec3 kTop{0.0f, 1.0f, 0.0f};
    constexpr glm::vec3 kBottom{0.0f, -1.0f, 0.0f};

    return {
        // Front (+Z)
        {{-h, -h, h}, kFront, kMaterialTint, glm::vec2{0.0f, 0.0f}},
        {{h, -h, h}, kFront, kMaterialTint, glm::vec2{1.0f, 0.0f}},
        {{h, h, h}, kFront, kMaterialTint, glm::vec2{1.0f, 1.0f}},
        {{-h, h, h}, kFront, kMaterialTint, glm::vec2{0.0f, 1.0f}},
        // Back (-Z)
        {{h, -h, -h}, kBack, kMaterialTint, glm::vec2{0.0f, 0.0f}},
        {{-h, -h, -h}, kBack, kMaterialTint, glm::vec2{1.0f, 0.0f}},
        {{-h, h, -h}, kBack, kMaterialTint, glm::vec2{1.0f, 1.0f}},
        {{h, h, -h}, kBack, kMaterialTint, glm::vec2{0.0f, 1.0f}},
        // Right (+X)
        {{h, -h, h}, kRight, kMaterialTint, glm::vec2{0.0f, 0.0f}},
        {{h, -h, -h}, kRight, kMaterialTint, glm::vec2{1.0f, 0.0f}},
        {{h, h, -h}, kRight, kMaterialTint, glm::vec2{1.0f, 1.0f}},
        {{h, h, h}, kRight, kMaterialTint, glm::vec2{0.0f, 1.0f}},
        // Left (-X)
        {{-h, -h, -h}, kLeft, kMaterialTint, glm::vec2{0.0f, 0.0f}},
        {{-h, -h, h}, kLeft, kMaterialTint, glm::vec2{1.0f, 0.0f}},
        {{-h, h, h}, kLeft, kMaterialTint, glm::vec2{1.0f, 1.0f}},
        {{-h, h, -h}, kLeft, kMaterialTint, glm::vec2{0.0f, 1.0f}},
        // Top (+Y)
        {{-h, h, h}, kTop, kMaterialTint, glm::vec2{0.0f, 0.0f}},
        {{h, h, h}, kTop, kMaterialTint, glm::vec2{1.0f, 0.0f}},
        {{h, h, -h}, kTop, kMaterialTint, glm::vec2{1.0f, 1.0f}},
        {{-h, h, -h}, kTop, kMaterialTint, glm::vec2{0.0f, 1.0f}},
        // Bottom (-Y)
        {{-h, -h, -h}, kBottom, kMaterialTint, glm::vec2{0.0f, 0.0f}},
        {{h, -h, -h}, kBottom, kMaterialTint, glm::vec2{1.0f, 0.0f}},
        {{h, -h, h}, kBottom, kMaterialTint, glm::vec2{1.0f, 1.0f}},
        {{-h, -h, h}, kBottom, kMaterialTint, glm::vec2{0.0f, 1.0f}},
    };
}

std::vector<uint32_t> Cube::indices() {
    std::vector<uint32_t> indices;
    indices.reserve(36);

    // Two triangles per face: (b, b+1, b+2) and (b+2, b+3, b),
    // counter-clockwise as seen from outside the face.
    for (uint32_t face = 0; face < 6; ++face) {
        const uint32_t base = face * 4;
        indices.insert(indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
    }
    return indices;
}

} // namespace engine
