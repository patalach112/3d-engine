#include "Graphics/UIRenderer.h"

#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/VulkanHelpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

#ifndef ENGINE_SHADER_DIR
#define ENGINE_SHADER_DIR "shaders"
#endif

namespace engine {
namespace {

UIScissorRect intersectScissor(const UIScissorRect& outer, const UIScissorRect& inner) {
    const float x0 = std::max(outer.x, inner.x);
    const float y0 = std::max(outer.y, inner.y);
    const float x1 = std::min(outer.x + outer.width, inner.x + inner.width);
    const float y1 = std::min(outer.y + outer.height, inner.y + inner.height);
    return UIScissorRect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

} // namespace

// ---------------------------------------------------------------------------
// UIBatch (pure CPU)
// ---------------------------------------------------------------------------

void UIBatch::begin(uint32_t viewportWidth, uint32_t viewportHeight) {
    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;
    clear();
}

void UIBatch::clear() {
    m_vertices.clear();
    m_indices.clear();
    m_segments.clear();
    m_scissors.assign({UIScissorRect{0.0f, 0.0f, static_cast<float>(m_viewportWidth),
                                     static_cast<float>(m_viewportHeight)}});
    m_scissorStack.assign({0});
    m_bounds = UIBounds{};
    m_textItems = 0;
    m_truncated = false;
}

void UIBatch::drawRect(float x, float y, float width, float height, const glm::vec4& color) {
    addQuad(x, y, width, height, UIRectStyle{}, color, kUIShapePrimitive, kUINoTexture, glm::vec4{0, 0, 1, 1});
}

void UIBatch::drawRoundedRect(float x, float y, float width, float height, float cornerRadius,
                              const glm::vec4& color) {
    UIRectStyle style{};
    style.cornerRadius = cornerRadius;
    addQuad(x, y, width, height, style, color, kUIShapePrimitive, kUINoTexture, glm::vec4{0, 0, 1, 1});
}

void UIBatch::drawRoundedRect(float x, float y, float width, float height, const UIRectStyle& style,
                              const glm::vec4& color) {
    addQuad(x, y, width, height, style, color, kUIShapePrimitive, kUINoTexture, glm::vec4{0, 0, 1, 1});
}

void UIBatch::drawTexture(UITextureId texture, float x, float y, float width, float height,
                          const glm::vec4& tint) {
    drawTexture(texture, x, y, width, height, glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}, tint);
}

void UIBatch::drawTexture(UITextureId texture, float x, float y, float width, float height,
                          const glm::vec4& uvRect, const glm::vec4& tint) {
    addQuad(x, y, width, height, UIRectStyle{}, tint, kUIImagePrimitive, texture, uvRect);
}

void UIBatch::drawText(std::string_view text, float x, float y, float scale, const glm::vec4& color) {
    if (text.empty()) {
        return;
    }
    ++m_textItems;
    if (m_fontAtlas == nullptr || scale <= 0.0f) {
        return; // accounting stub until a font atlas is set
    }
    const SdfFontAtlas& atlas = *m_fontAtlas;
    const float advance = atlas.advance * scale;
    const float cellWidth = atlas.cellWidth * scale;
    const float cellHeight = atlas.cellHeight * scale;
    const float inset = atlas.cellInset * scale;
    float penX = x;
    float lineY = y;
    for (const char character : text) {
        if (character == '\n') {
            penX = x;
            lineY += atlas.lineHeight * scale;
            continue;
        }
        if (character == '\r') {
            continue;
        }
        if (character != ' ') {
            const SdfGlyph& glyph = atlas.glyphFor(character);
            // The quad covers the whole atlas cell (glyph box plus the
            // SDF padding halo), inset back from the pen position.
            addQuad(penX - inset, lineY - inset, cellWidth, cellHeight, UIRectStyle{}, color,
                    kUITextPrimitive, kUIFontTexture, glm::vec4{glyph.u0, glyph.v0, glyph.u1, glyph.v1});
        }
        penX += advance;
    }
}

float UIBatch::measureText(std::string_view text, float scale) const {
    if (m_fontAtlas == nullptr || scale <= 0.0f) {
        return 0.0f;
    }
    float widest = 0.0f;
    float line = 0.0f;
    for (const char character : text) {
        if (character == '\n') {
            widest = std::max(widest, line);
            line = 0.0f;
        } else if (character != '\r') {
            line += m_fontAtlas->advance * scale;
        }
    }
    return std::max(widest, line);
}

void UIBatch::pushScissor(float x, float y, float width, float height) {
    const UIScissorRect requested{x, y, std::max(0.0f, width), std::max(0.0f, height)};
    m_scissors.push_back(intersectScissor(currentScissor(), requested));
    m_scissorStack.push_back(static_cast<uint32_t>(m_scissors.size() - 1));
}

void UIBatch::popScissor() {
    if (m_scissorStack.size() > 1) {
        m_scissorStack.pop_back();
    }
}

UIScissorRect UIBatch::currentScissor() const {
    return m_scissors[m_scissorStack.back()];
}

void UIBatch::addQuad(float x, float y, float width, float height, const UIRectStyle& style,
                      const glm::vec4& color, float kind, UITextureId texture, const glm::vec4& uvRect) {
    if (width <= 0.0f || height <= 0.0f) {
        return; // degenerate primitive: nothing to rasterize
    }
    if (m_vertices.size() + 4 > kMaxVertices || m_indices.size() + 6 > kMaxIndices) {
        m_truncated = true; // drop whole quads; never emit a partial one
        return;
    }
    const float halfShortSide = 0.5f * std::min(width, height);
    const float radius = std::clamp(style.cornerRadius, 0.0f, halfShortSide);
    const float border = std::clamp(style.borderWidth, 0.0f, halfShortSide);
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const uint32_t scissor = m_scissorStack.back();

    if (m_segments.empty() || m_segments.back().texture != texture || m_segments.back().scissor != scissor) {
        m_segments.push_back(
            UISegment{static_cast<uint32_t>(m_indices.size()), 0, scissor, texture});
    }
    m_segments.back().indexCount += 6;

    const auto corner = [&](float px, float py, float u, float v) {
        UIVertex vertex{};
        vertex.position = {px, py};
        vertex.uv = {u, v};
        vertex.color = color;
        vertex.cornerRadius = radius;
        vertex.borderWidth = border;
        vertex.borderColor = style.borderColor;
        vertex.uvRect = uvRect;
        vertex.kind = kind;
        m_vertices.push_back(vertex);
    };
    corner(x, y, 0.0f, 0.0f);                  // top-left
    corner(x + width, y, 1.0f, 0.0f);          // top-right
    corner(x + width, y + height, 1.0f, 1.0f); // bottom-right
    corner(x, y + height, 0.0f, 1.0f);         // bottom-left
    m_indices.insert(m_indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});

    if (m_bounds.empty) {
        m_bounds.minX = x;
        m_bounds.minY = y;
        m_bounds.maxX = x + width;
        m_bounds.maxY = y + height;
        m_bounds.empty = false;
    } else {
        m_bounds.minX = std::min(m_bounds.minX, x);
        m_bounds.minY = std::min(m_bounds.minY, y);
        m_bounds.maxX = std::max(m_bounds.maxX, x + width);
        m_bounds.maxY = std::max(m_bounds.maxY, y + height);
    }
}

// ---------------------------------------------------------------------------
// UIRenderer (Vulkan)
// ---------------------------------------------------------------------------

UIRenderer::VertexInput UIRenderer::vertexInput() {
    VertexInput input{};
    input.binding.binding = 0;
    input.binding.stride = sizeof(UIVertex);
    input.binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const auto attribute = [](uint32_t location, VkFormat format, size_t offset) {
        VkVertexInputAttributeDescription description{};
        description.binding = 0;
        description.location = location;
        description.format = format;
        description.offset = static_cast<uint32_t>(offset);
        return description;
    };
    input.attributes[0] = attribute(0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, position));
    input.attributes[1] = attribute(1, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, uv));
    input.attributes[2] = attribute(2, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UIVertex, color));
    input.attributes[3] = attribute(3, VK_FORMAT_R32_SFLOAT, offsetof(UIVertex, cornerRadius));
    input.attributes[4] = attribute(4, VK_FORMAT_R32_SFLOAT, offsetof(UIVertex, borderWidth));
    input.attributes[5] = attribute(5, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UIVertex, borderColor));
    input.attributes[6] = attribute(6, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UIVertex, uvRect));
    input.attributes[7] = attribute(7, VK_FORMAT_R32_SFLOAT, offsetof(UIVertex, kind));
    return input;
}

UIRenderer::UIRenderer(VulkanContext& context, const Swapchain& swapchain) : m_context(context) {
    createBuffers();
    m_fontAtlas = generateSdfFontAtlas();
    m_batch.setFontAtlas(&m_fontAtlas);
    createFontAtlasTexture();
    createPipeline(swapchain);
    createFontDescriptorSet();
}

UIRenderer::~UIRenderer() {
    const VkDevice device = m_context.device();
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    }
    if (m_atlasSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_atlasSampler, nullptr);
    }
    if (m_atlasView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_atlasView, nullptr);
    }
    if (m_atlasImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_atlasImage, nullptr);
    }
    if (m_atlasMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_atlasMemory, nullptr);
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_layout, nullptr);
    }
}

UITextureId UIRenderer::registerTexture(VkDescriptorSet descriptorSet) {
    if (descriptorSet == VK_NULL_HANDLE) {
        return kUINoTexture;
    }
    const UITextureId id = m_nextTexture++;
    m_textures[id] = descriptorSet;
    return id;
}

void UIRenderer::unregisterTexture(UITextureId texture) {
    if (texture > kUIFontTexture) {
        m_textures.erase(texture);
    }
}

VkDescriptorSet UIRenderer::resolveDescriptorSet(UITextureId texture) const {
    if (texture == kUINoTexture || texture == kUIFontTexture) {
        return m_fontDescriptorSet;
    }
    const auto it = m_textures.find(texture);
    return it != m_textures.end() ? it->second : m_fontDescriptorSet;
}

void UIRenderer::record(VkCommandBuffer commandBuffer, VkExtent2D extent) {
    const std::vector<UIVertex>& vertices = m_batch.vertices();
    const std::vector<uint32_t>& indices = m_batch.indices();
    const std::vector<UISegment>& segments = m_batch.segments();
    if (segments.empty() || extent.width == 0 || extent.height == 0) {
        m_batch.clear();
        return;
    }
    if (m_vertexBuffer.mappedData() != nullptr) {
        std::memcpy(m_vertexBuffer.mappedData(), vertices.data(), vertices.size() * sizeof(UIVertex));
    }
    if (m_indexBuffer.mappedData() != nullptr) {
        std::memcpy(m_indexBuffer.mappedData(), indices.data(), indices.size() * sizeof(uint32_t));
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const VkBuffer vertexBuffers[] = {m_vertexBuffer.handle()};
    constexpr VkDeviceSize vertexOffsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, vertexOffsets);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
    const std::array<float, 2> viewportSize{static_cast<float>(extent.width),
                                            static_cast<float>(extent.height)};
    vkCmdPushConstants(commandBuffer, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       static_cast<uint32_t>(viewportSize.size() * sizeof(float)), viewportSize.data());

    for (const UISegment& segment : segments) {
        const UIScissorRect& rect = m_batch.scissors()[segment.scissor];
        const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(rect.x)));
        const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(rect.y)));
        const int32_t x1 = std::min(static_cast<int32_t>(extent.width),
                                    static_cast<int32_t>(std::ceil(rect.x + rect.width)));
        const int32_t y1 = std::min(static_cast<int32_t>(extent.height),
                                    static_cast<int32_t>(std::ceil(rect.y + rect.height)));
        VkRect2D scissor{};
        scissor.offset = {x0, y0};
        scissor.extent = {static_cast<uint32_t>(std::max(0, x1 - x0)),
                          static_cast<uint32_t>(std::max(0, y1 - y0))};
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        VkDescriptorSet descriptorSet = resolveDescriptorSet(segment.texture);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1,
                                &descriptorSet, 0, nullptr);
        vkCmdDrawIndexed(commandBuffer, segment.indexCount, 1, segment.firstIndex, 0, 0);
    }

    // Restore the full-frame scissor for the passes that follow (HUD).
    VkRect2D fullScissor{};
    fullScissor.offset = {0, 0};
    fullScissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &fullScissor);

    m_batch.clear(); // per-frame batch: flushed, next frame starts empty
}

void UIRenderer::createBuffers() {
    m_vertexBuffer = Buffer(m_context, static_cast<VkDeviceSize>(UIBatch::kMaxVertices) * sizeof(UIVertex),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_vertexBuffer.map(); // persistently mapped; rewritten every flush
    m_indexBuffer = Buffer(m_context, static_cast<VkDeviceSize>(UIBatch::kMaxIndices) * sizeof(uint32_t),
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_indexBuffer.map();
}

void UIRenderer::createFontAtlasTexture() {
    const VkDevice device = m_context.device();
    Buffer staging(m_context, static_cast<VkDeviceSize>(m_fontAtlas.pixels.size()),
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.map();
    std::memcpy(staging.mappedData(), m_fontAtlas.pixels.data(), m_fontAtlas.pixels.size());
    staging.unmap();

    helpers::createImage(m_context, m_fontAtlas.width, m_fontAtlas.height, VK_FORMAT_R8_UNORM,
                         VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_atlasImage, m_atlasMemory);

    VkCommandBuffer commandBuffer = helpers::beginSingleTimeCommands(m_context);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_atlasImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_fontAtlas.width, m_fontAtlas.height, 1};
    vkCmdCopyBufferToImage(commandBuffer, staging.handle(), m_atlasImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    helpers::endSingleTimeCommands(m_context, commandBuffer);

    m_atlasView = helpers::createImageView(m_context, m_atlasImage, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_atlasSampler) != VK_SUCCESS) {
        throw std::runtime_error("UIRenderer: failed to create font atlas sampler");
    }
}

void UIRenderer::createFontDescriptorSet() {
    const VkDevice device = m_context.device();
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("UIRenderer: failed to create descriptor pool");
    }

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(device, &allocateInfo, &m_fontDescriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("UIRenderer: failed to allocate font descriptor set");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = m_atlasSampler;
    imageInfo.imageView = m_atlasView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_fontDescriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

VkShaderModule UIRenderer::createShaderModule(const char* fileName) const {
    const std::string path = std::string(ENGINE_SHADER_DIR) + "/" + fileName;
    const std::vector<char> code = helpers::readFile(path);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_context.device(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("UIRenderer: failed to create shader module from " + path);
    }
    return shaderModule;
}

void UIRenderer::createPipeline(const Swapchain& swapchain) {
    const VkDevice device = m_context.device();

    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &samplerBinding;
    if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &m_setLayout) != VK_SUCCESS) {
        throw std::runtime_error("UIRenderer: failed to create descriptor set layout");
    }

    VkShaderModule vertexShader = createShaderModule("ui.vert.spv");
    VkShaderModule fragmentShader = createShaderModule("ui.frag.spv");

    VkPipelineShaderStageCreateInfo vertexStage{};
    vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertexShader;
    vertexStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragmentStage{};
    fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragmentShader;
    fragmentStage.pName = "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {vertexStage, fragmentStage};

    const VertexInput input = vertexInput();
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &input.binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(input.attributes.size());
    vertexInput.pVertexAttributeDescriptions = input.attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Overlay: never occluded by scene depth.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = 2 * sizeof(float); // viewport size

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertexShader, nullptr);
        vkDestroyShaderModule(device, fragmentShader, nullptr);
        throw std::runtime_error("UIRenderer: failed to create pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_layout;
    pipelineInfo.renderPass = swapchain.renderPass();
    pipelineInfo.subpass = 0;

    const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, vertexShader, nullptr);
    vkDestroyShaderModule(device, fragmentShader, nullptr);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("UIRenderer: failed to create graphics pipeline");
    }
}

} // namespace engine
