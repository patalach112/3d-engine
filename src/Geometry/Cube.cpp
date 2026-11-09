#include "Geometry/Cube.h"

namespace engine {

std::vector<Vertex> Cube::vertices() {
    constexpr float h = 0.5f; // half extent

    // One uniform material tint for every face. The sandbox uses a single
    // professional material (its color lives in MaterialDesc::baseColor);
    // per-face variation comes only from normal-based diffuse lighting in
    // the shader, never from debug face colors.
    constexpr glm::vec3 kMaterialTint{1.0f, 1.0f, 1.0f};

    constexpr glm::vec3 kFront{0.0f, 0.0f, 1.0f};
    constexpr glm::vec3 kBack{0.0f, 0.0f, -1.0f};
    constexpr glm::vec3 kRight{1.0f, 0.0f, 0.0f};
    constexpr glm::vec3 kLeft{-1.0f, 0.0f, 0.0f};
    constexpr glm::vec3 kTop{0.0f, 1.0f, 0.0f};
    constexpr glm::vec3 kBottom{0.0f, -1.0f, 0.0f};

    return {
        // Front (+Z)
        {{-h, -h, h}, kFront, kMaterialTint},
        {{h, -h, h}, kFront, kMaterialTint},
        {{h, h, h}, kFront, kMaterialTint},
        {{-h, h, h}, kFront, kMaterialTint},
        // Back (-Z)
        {{h, -h, -h}, kBack, kMaterialTint},
        {{-h, -h, -h}, kBack, kMaterialTint},
        {{-h, h, -h}, kBack, kMaterialTint},
        {{h, h, -h}, kBack, kMaterialTint},
        // Right (+X)
        {{h, -h, h}, kRight, kMaterialTint},
        {{h, -h, -h}, kRight, kMaterialTint},
        {{h, h, -h}, kRight, kMaterialTint},
        {{h, h, h}, kRight, kMaterialTint},
        // Left (-X)
        {{-h, -h, -h}, kLeft, kMaterialTint},
        {{-h, -h, h}, kLeft, kMaterialTint},
        {{-h, h, h}, kLeft, kMaterialTint},
        {{-h, h, -h}, kLeft, kMaterialTint},
        // Top (+Y)
        {{-h, h, h}, kTop, kMaterialTint},
        {{h, h, h}, kTop, kMaterialTint},
        {{h, h, -h}, kTop, kMaterialTint},
        {{-h, h, -h}, kTop, kMaterialTint},
        // Bottom (-Y)
        {{-h, -h, -h}, kBottom, kMaterialTint},
        {{h, -h, -h}, kBottom, kMaterialTint},
        {{h, -h, h}, kBottom, kMaterialTint},
        {{-h, -h, h}, kBottom, kMaterialTint},
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
