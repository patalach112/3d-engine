#include "Geometry/Cube.h"

namespace engine {

std::vector<Vertex> Cube::vertices() {
    constexpr float h = 0.5f; // half extent

    // One distinct color per face.
    constexpr glm::vec3 kRed{1.0f, 0.25f, 0.25f};
    constexpr glm::vec3 kGreen{0.25f, 1.0f, 0.35f};
    constexpr glm::vec3 kBlue{0.30f, 0.45f, 1.0f};
    constexpr glm::vec3 kYellow{1.0f, 0.90f, 0.25f};
    constexpr glm::vec3 kMagenta{1.0f, 0.30f, 0.90f};
    constexpr glm::vec3 kCyan{0.25f, 0.95f, 1.0f};

    return {
        // Front (+Z) — red
        {{-h, -h, h}, kRed},
        {{h, -h, h}, kRed},
        {{h, h, h}, kRed},
        {{-h, h, h}, kRed},
        // Back (-Z) — green
        {{h, -h, -h}, kGreen},
        {{-h, -h, -h}, kGreen},
        {{-h, h, -h}, kGreen},
        {{h, h, -h}, kGreen},
        // Right (+X) — blue
        {{h, -h, h}, kBlue},
        {{h, -h, -h}, kBlue},
        {{h, h, -h}, kBlue},
        {{h, h, h}, kBlue},
        // Left (-X) — yellow
        {{-h, -h, -h}, kYellow},
        {{-h, -h, h}, kYellow},
        {{-h, h, h}, kYellow},
        {{-h, h, -h}, kYellow},
        // Top (+Y) — magenta
        {{-h, h, h}, kMagenta},
        {{h, h, h}, kMagenta},
        {{h, h, -h}, kMagenta},
        {{-h, h, -h}, kMagenta},
        // Bottom (-Y) — cyan
        {{-h, -h, -h}, kCyan},
        {{h, -h, -h}, kCyan},
        {{h, -h, h}, kCyan},
        {{-h, -h, h}, kCyan},
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
