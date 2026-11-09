#pragma once

#include <vulkan/vulkan.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>

namespace engine {

// Interleaved vertex format: position + face normal + tint + UV. The
// normal is what lets the fragment shader shade each face by its true
// orientation (diffuse lighting) instead of presenting flat debug
// colors; the tint is multiplied by the per-draw material color. The UV
// is carried end-to-end from imported meshes (OBJ `vt` records) so the
// vertex format is texture-ready even though the current sandbox
// materials are untextured.
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
    glm::vec2 texCoord{0.0f};

    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription description{};
        description.binding = 0;
        description.stride = sizeof(Vertex);
        description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return description;
    }

    static std::array<VkVertexInputAttributeDescription, 4> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 4> descriptions{};

        descriptions[0].binding = 0;
        descriptions[0].location = 0;
        descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        descriptions[0].offset = offsetof(Vertex, position);

        descriptions[1].binding = 0;
        descriptions[1].location = 1;
        descriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        descriptions[1].offset = offsetof(Vertex, normal);

        descriptions[2].binding = 0;
        descriptions[2].location = 2;
        descriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        descriptions[2].offset = offsetof(Vertex, color);

        descriptions[3].binding = 0;
        descriptions[3].location = 3;
        descriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
        descriptions[3].offset = offsetof(Vertex, texCoord);

        return descriptions;
    }
};

} // namespace engine
