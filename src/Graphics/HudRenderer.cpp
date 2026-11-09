#include "Graphics/HudRenderer.h"

#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/VulkanHelpers.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifndef ENGINE_SHADER_DIR
#define ENGINE_SHADER_DIR "shaders"
#endif

namespace engine {
namespace {

// Generous cap: ~90k font pixels per refresh. Text beyond this is
// truncated rather than reallocating the buffer mid-frame.
constexpr uint32_t kMaxHudVertices = 65536;
constexpr float kHudOriginPixels = 12.0f;
constexpr float kHudPixelScale = 2.0f; // 10x14 px glyphs

} // namespace

HudRenderer::HudRenderer(VulkanContext& context, const Swapchain& swapchain) : m_context(context) {
    createVertexBuffer();
    createPipeline(swapchain);
}

HudRenderer::~HudRenderer() {
    const VkDevice device = m_context.device();
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_layout, nullptr);
    }
}

void HudRenderer::setText(std::string text) {
    if (text != m_text) {
        m_text = std::move(text);
        m_dirty = true;
    }
}

void HudRenderer::record(VkCommandBuffer commandBuffer, VkExtent2D extent) {
    if (m_text.empty() || extent.width == 0 || extent.height == 0) {
        return;
    }
    if (m_dirty || extent.width != m_builtExtent.width || extent.height != m_builtExtent.height) {
        rebuildVertices(extent);
    }
    if (m_vertexCount == 0) {
        return;
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const VkBuffer buffers[] = {m_vertexBuffer.handle()};
    constexpr VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);
    vkCmdDraw(commandBuffer, m_vertexCount, 1, 0, 0);
}

void HudRenderer::rebuildVertices(VkExtent2D extent) {
    const std::vector<HudVertex> vertices = buildHudVertices(m_text, kHudOriginPixels, kHudOriginPixels,
                                                             kHudPixelScale, extent.width, extent.height);
    const size_t count = std::min(vertices.size(), static_cast<size_t>(kMaxHudVertices));
    if (count > 0 && m_vertexBuffer.mappedData() != nullptr) {
        std::memcpy(m_vertexBuffer.mappedData(), vertices.data(), count * sizeof(HudVertex));
    }
    m_vertexCount = static_cast<uint32_t>(count);
    m_builtExtent = extent;
    m_dirty = false;
}

void HudRenderer::createVertexBuffer() {
    m_vertexBuffer = Buffer(m_context, static_cast<VkDeviceSize>(kMaxHudVertices) * sizeof(HudVertex),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_vertexBuffer.map(); // persistently mapped; rewritten only on text changes
}

VkShaderModule HudRenderer::createShaderModule(const char* fileName) const {
    const std::string path = std::string(ENGINE_SHADER_DIR) + "/" + fileName;
    const std::vector<char> code = helpers::readFile(path);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_context.device(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("HudRenderer: failed to create shader module from " + path);
    }
    return shaderModule;
}

void HudRenderer::createPipeline(const Swapchain& swapchain) {
    const VkDevice device = m_context.device();

    VkShaderModule vertexShader = createShaderModule("hud.vert.spv");
    VkShaderModule fragmentShader = createShaderModule("hud.frag.spv");

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

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(HudVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = static_cast<uint32_t>(offsetof(HudVertex, x));
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[1].offset = static_cast<uint32_t>(offsetof(HudVertex, r));

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

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

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertexShader, nullptr);
        vkDestroyShaderModule(device, fragmentShader, nullptr);
        throw std::runtime_error("HudRenderer: failed to create pipeline layout");
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
        throw std::runtime_error("HudRenderer: failed to create graphics pipeline");
    }
}

} // namespace engine
