#pragma once

#include <vulkan/vulkan.h>

#include <glm/vec3.hpp>

#include <array>

namespace engine {

// Interleaved vertex format: position + face normal + tint. The normal is
// what lets the fragment shader shade each face by its true orientation
// (diffuse lighting) instead of presenting flat debug colors; the tint is
// multiplied by the per-draw material color.
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;

    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription description{};
        description.binding = 0;
        description.stride = sizeof(Vertex);
        description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return description;
    }

    static std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> descriptions{};

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

        return descriptions;
    }
};

} // namespace engine
