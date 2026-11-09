#include "Graphics/GraphicsPipeline.h"

#include "Geometry/Vertex.h"
#include "Graphics/RenderQueue.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/VulkanHelpers.h"

#include <array>
#include <glm/mat4x4.hpp>
#include <unordered_set>
#include <stdexcept>
#include <string>

#ifndef ENGINE_SHADER_DIR
#define ENGINE_SHADER_DIR "shaders"
#endif

namespace engine {

namespace {

VkPrimitiveTopology toVulkan(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::TriangleList: break;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkPolygonMode toVulkan(PolygonMode mode) {
    return mode == PolygonMode::Line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
}

VkCullModeFlags toVulkan(CullMode mode) {
    switch (mode) {
    case CullMode::None: return VK_CULL_MODE_NONE;
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back: break;
    }
    return VK_CULL_MODE_BACK_BIT;
}

VkCompareOp toVulkan(DepthCompare compare) {
    switch (compare) {
    case DepthCompare::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case DepthCompare::Equal: return VK_COMPARE_OP_EQUAL;
    case DepthCompare::Always: return VK_COMPARE_OP_ALWAYS;
    case DepthCompare::Never: return VK_COMPARE_OP_NEVER;
    case DepthCompare::Less: break;
    }
    return VK_COMPARE_OP_LESS;
}

uint64_t pipelineKey(const PipelineStateDesc& state, const std::string& vertexShaderFile,
                     const std::string& fragmentShaderFile) {
    uint64_t hash = hashPipelineState(state);
    for (const std::string& name : {vertexShaderFile, fragmentShaderFile}) {
        for (const unsigned char c : name) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        hash ^= 0xFFu;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

GraphicsPipeline::GraphicsPipeline(VulkanContext& context, const Swapchain& swapchain) : m_context(context) {
    createDescriptorSetLayout();
    createPipelineCache();
    m_pipeline = createPipeline(swapchain, PipelineStateDesc{}, "cube.vert.spv", "cube.frag.spv");
    m_stateCache.intern(PipelineStateDesc{});
    m_pipelinesByState.emplace(pipelineKey(PipelineStateDesc{}, "cube.vert.spv", "cube.frag.spv"), m_pipeline);
    m_distinctPipelines = 1;
}

GraphicsPipeline::~GraphicsPipeline() {
    const VkDevice device = m_context.device();
    // Destroy every distinct pipeline exactly once (materials may share).
    std::unordered_set<VkPipeline> destroyed;
    const auto destroyOnce = [&destroyed, device](VkPipeline pipeline) {
        if (pipeline != VK_NULL_HANDLE && destroyed.insert(pipeline).second) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
    };
    destroyOnce(m_pipeline);
    for (const auto& [materialId, pipeline] : m_materialPipelines) {
        (void)materialId;
        destroyOnce(pipeline);
    }
    if (m_pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device, m_pipelineCache, nullptr);
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_layout, nullptr);
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
    }
}

void GraphicsPipeline::bind(VkCommandBuffer commandBuffer) const {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
}

VkPipeline GraphicsPipeline::pipelineFor(uint32_t materialId) const {
    const auto found = m_materialPipelines.find(materialId);
    return found != m_materialPipelines.end() ? found->second : VK_NULL_HANDLE;
}

VkPipeline GraphicsPipeline::getOrCreatePipeline(uint32_t materialId, const PipelineStateDesc& state,
                                                 const std::string& vertexShaderFile,
                                                 const std::string& fragmentShaderFile,
                                                 const Swapchain& swapchain) {
    if (const VkPipeline existing = pipelineFor(materialId); existing != VK_NULL_HANDLE) {
        return existing;
    }
    // State deduplication: an identical (state, shader pair) combination
    // reuses the existing VkPipeline; only genuinely new combinations
    // pay a creation (accelerated by the VkPipelineCache).
    m_stateCache.intern(state);
    const uint64_t key = pipelineKey(state, vertexShaderFile, fragmentShaderFile);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (const auto found = m_pipelinesByState.find(key); found != m_pipelinesByState.end()) {
        pipeline = found->second;
    } else {
        pipeline = createPipeline(swapchain, state, vertexShaderFile, fragmentShaderFile);
        m_pipelinesByState.emplace(key, pipeline);
        ++m_distinctPipelines;
    }
    m_materialPipelines.emplace(materialId, pipeline);
    return pipeline;
}

void GraphicsPipeline::createPipelineCache() {
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (vkCreatePipelineCache(m_context.device(), &cacheInfo, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        throw std::runtime_error("GraphicsPipeline: failed to create pipeline cache");
    }
}

void GraphicsPipeline::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    uboBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboBinding;

    if (vkCreateDescriptorSetLayout(m_context.device(), &layoutInfo, nullptr, &m_descriptorSetLayout) !=
        VK_SUCCESS) {
        throw std::runtime_error("GraphicsPipeline: failed to create descriptor set layout");
    }
}

VkShaderModule GraphicsPipeline::createShaderModule(const char* fileName) const {
    const std::string path = std::string(ENGINE_SHADER_DIR) + "/" + fileName;
    const std::vector<char> code = helpers::readFile(path);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_context.device(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("GraphicsPipeline: failed to create shader module from " + path);
    }
    return shaderModule;
}

VkPipeline GraphicsPipeline::createPipeline(const Swapchain& swapchain, const PipelineStateDesc& state,
                                            const std::string& vertexShaderFile,
                                            const std::string& fragmentShaderFile) {
    const VkDevice device = m_context.device();

    VkShaderModule vertexShader = createShaderModule(vertexShaderFile.c_str());
    VkShaderModule fragmentShader = createShaderModule(fragmentShaderFile.c_str());

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

    // --- Vertex input -------------------------------------------------------
    const VkVertexInputBindingDescription bindingDescription = Vertex::bindingDescription();
    const auto attributeDescriptions = Vertex::attributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDescription;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    vertexInput.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = toVulkan(state.topology);
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // --- Viewport / scissor (dynamic: set per frame, resize-proof) ----------
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                         VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // --- Rasterizer ---------------------------------------------------------
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = toVulkan(state.polygonMode);
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = toVulkan(state.cullMode);
    // GLM's projection flips the framebuffer Y axis, which reverses triangle
    // winding on screen. Declaring clockwise front faces keeps back-face
    // culling correct for geometry wound CCW in model space.
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampling.sampleShadingEnable = VK_FALSE;

    // --- Depth testing (required for correct 3D occlusion) ------------------
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = state.depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = state.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = toVulkan(state.depthCompare);
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // --- Color blending: opaque, no blending --------------------------------
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = state.blendEnable ? VK_TRUE : VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // --- Pipeline layout ------------------------------------------------------
    // Per-entity model matrices and material colors arrive as push
    // constants (80 bytes, inside the guaranteed 128-byte minimum). The
    // default pipeline state is a solid material: depth test + depth write
    // enabled with Less comparison and back-face culling, so nearer solid
    // geometry always occludes what is behind it.
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(DrawPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (m_layout == VK_NULL_HANDLE &&
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertexShader, nullptr);
        vkDestroyShaderModule(device, fragmentShader, nullptr);
        throw std::runtime_error("GraphicsPipeline: failed to create pipeline layout");
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

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateGraphicsPipelines(device, m_pipelineCache, 1, &pipelineInfo, nullptr, &pipeline);

    // Shader modules are baked into the pipeline and can be released.
    vkDestroyShaderModule(device, vertexShader, nullptr);
    vkDestroyShaderModule(device, fragmentShader, nullptr);

    if (result != VK_SUCCESS) {
        throw std::runtime_error("GraphicsPipeline: failed to create graphics pipeline");
    }
    return pipeline;
}

} // namespace engine
