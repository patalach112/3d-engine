#include "Graphics/GraphicsPipeline.h"
#include "Graphics/ShadowMap.h"

#include "Core/Jobs/JobSystem.h"
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

VkFrontFace toVulkan(FrontFace frontFace) {
    return frontFace == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
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

// Thread-safe SPIR-V store filled by preloadShaderAsync workers and
// consumed by createShaderModule on the main thread. Entries are
// never erased: the shader set is tiny and fixed at startup.
struct GraphicsPipeline::ShaderBytecodeCache {
    std::mutex mutex;
    std::unordered_map<std::string, std::vector<char>> bytes;
    std::unordered_set<std::string> inFlight;
};

GraphicsPipeline::GraphicsPipeline(VulkanContext& context, const Swapchain& swapchain)
    : m_shaderCache(std::make_shared<ShaderBytecodeCache>()), m_context(context) {
    createDescriptorSetLayout();
    createPipelineCache();
    m_pipeline = createPipeline(swapchain.renderPass(), PipelineStateDesc{}, "cube.vert.spv", "cube.frag.spv");
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
    destroyOnce(m_shadowPipeline);
    destroyOnce(m_skyboxPipeline);
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
        const VkRenderPass pass = m_sceneRenderPass != VK_NULL_HANDLE ? m_sceneRenderPass : swapchain.renderPass();
        pipeline = createPipeline(pass, state, vertexShaderFile, fragmentShaderFile);
        m_pipelinesByState.emplace(key, pipeline);
        ++m_distinctPipelines;
    }
    m_materialPipelines.emplace(materialId, pipeline);
    return pipeline;
}

bool GraphicsPipeline::queuePipelineWarmup(uint32_t materialId, const PipelineStateDesc& state,
                                           const std::string& vertexShaderFile,
                                           const std::string& fragmentShaderFile) {
    if (pipelineFor(materialId) != VK_NULL_HANDLE) {
        return false; // already compiled: nothing to warm up
    }
    return m_warmup.enqueue(
        PipelineWarmupRequest{materialId, state, vertexShaderFile, fragmentShaderFile});
}

size_t GraphicsPipeline::processPipelineWarmup(size_t maxCreations, const Swapchain& swapchain) {
    if (maxCreations == 0 || m_warmup.empty()) {
        return 0;
    }
    size_t created = 0;
    std::vector<PipelineWarmupRequest> batch;
    // Drain one request at a time: a request whose state already has
    // a pipeline binds for free, so the loop only stops once the
    // DISTINCT-creation budget for this frame is spent.
    while (!m_warmup.empty()) {
        batch.clear();
        m_warmup.drain(1, batch);
        const PipelineWarmupRequest& request = batch.front();
        if (pipelineFor(request.materialId) != VK_NULL_HANDLE) {
            continue; // created synchronously in the meantime
        }
        const size_t before = m_distinctPipelines;
        getOrCreatePipeline(request.materialId, request.state, request.vertexShader, request.fragmentShader,
                            swapchain);
        if (m_distinctPipelines > before) {
            ++created;
            if (created >= maxCreations) {
                break;
            }
        }
    }
    return created;
}

void GraphicsPipeline::preloadShaderAsync(JobSystem& jobs, const std::string& shaderFile) {
    const std::shared_ptr<ShaderBytecodeCache> cache = m_shaderCache;
    {
        std::lock_guard lock(cache->mutex);
        if (cache->bytes.find(shaderFile) != cache->bytes.end() ||
            !cache->inFlight.insert(shaderFile).second) {
            return; // cached already, or a worker is on it
        }
    }
    const std::string path = std::string(ENGINE_SHADER_DIR) + "/" + shaderFile;
    jobs.submit([cache, shaderFile, path] {
        std::vector<char> code;
        try {
            code = helpers::readFile(path);
        } catch (...) {
            // Leave the cache empty: the main-thread creation path
            // re-reads the file and reports the failure properly.
            std::lock_guard lock(cache->mutex);
            cache->inFlight.erase(shaderFile);
            return;
        }
        std::lock_guard lock(cache->mutex);
        cache->bytes.emplace(shaderFile, std::move(code));
        cache->inFlight.erase(shaderFile);
    });
}

size_t GraphicsPipeline::preloadedShaderCount() const {
    std::lock_guard lock(m_shaderCache->mutex);
    return m_shaderCache->bytes.size();
}

void GraphicsPipeline::createPipelineCache() {
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (vkCreatePipelineCache(m_context.device(), &cacheInfo, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        throw std::runtime_error("GraphicsPipeline: failed to create pipeline cache");
    }
}

void GraphicsPipeline::createDescriptorSetLayout() {
    // Binding 0: per-frame uniforms (camera view/proj + light-space
    // matrix and direction). Read by the vertex shader (camera/light
    // transforms) and the fragment shader (shadow reprojection).
    // Binding 1: the shadow-map depth texture, sampled in the fragment
    // shader only.
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = 1;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = kMaxShadowCascades; // cascade depth array (Phase 21)
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    shadowBinding.pImmutableSamplers = nullptr;

    // Phase 22 bindings (all fragment-side): the material texture
    // array, the image-based-lighting set (irradiance / prefiltered
    // specular / BRDF LUT) and the raw environment cube the skybox
    // samples.
    const auto samplerBinding = [](uint32_t binding, uint32_t count) {
        VkDescriptorSetLayoutBinding result{};
        result.binding = binding;
        result.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        result.descriptorCount = count;
        result.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        result.pImmutableSamplers = nullptr;
        return result;
    };
    const std::array<VkDescriptorSetLayoutBinding, 9> bindings = {
        uboBinding, shadowBinding, samplerBinding(2, kMaxMaterialTextures), samplerBinding(3, 1),
        samplerBinding(4, 1),      samplerBinding(5, 1),                    samplerBinding(6, 1),
        samplerBinding(7, 1), // Phase 32: spot shadow map
        samplerBinding(8, 1)}; // point shadow cube (2D array of 6 faces)

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_context.device(), &layoutInfo, nullptr, &m_descriptorSetLayout) !=
        VK_SUCCESS) {
        throw std::runtime_error("GraphicsPipeline: failed to create descriptor set layout");
    }
}

VkPipeline GraphicsPipeline::createShadowPipeline(VkRenderPass shadowRenderPass) {
    if (m_shadowPipeline != VK_NULL_HANDLE) {
        return m_shadowPipeline;
    }
    const VkDevice device = m_context.device();

    VkShaderModule vertexShader = createShaderModule("shadow.vert.spv");

    VkPipelineShaderStageCreateInfo vertexStage{};
    vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertexShader;
    vertexStage.pName = "main";

    const VkVertexInputBindingDescription bindingDescription = Vertex::bindingDescription();
    const auto attributeDescriptions = Vertex::attributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDescription;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInput.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Shadow rasterizer: render back faces into the depth map (front-face
    // culling) so the stored depth sits slightly inside each caster,
    // which together with the slope-scaled shader bias removes acne
    // without peter-panning. Front faces keep the same winding
    // convention as the main pass (counter-clockwise, kEngineFrontFace).
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;
    rasterizer.frontFace = toVulkan(kEngineFrontFace);
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.0f;
    rasterizer.depthBiasSlopeFactor = 1.5f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampling.sampleShadingEnable = VK_FALSE;

    // Depth-only pass: strict Less test + depth write, no color output.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 0;
    colorBlending.pAttachments = nullptr;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 1;
    pipelineInfo.pStages = &vertexStage;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_layout;
    pipelineInfo.renderPass = shadowRenderPass;
    pipelineInfo.subpass = 0;

    const VkResult result =
        vkCreateGraphicsPipelines(device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_shadowPipeline);
    vkDestroyShaderModule(device, vertexShader, nullptr);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("GraphicsPipeline: failed to create shadow pipeline");
    }
    return m_shadowPipeline;
}

void GraphicsPipeline::setSceneRenderPass(VkRenderPass sceneRenderPass) {
    m_sceneRenderPass = sceneRenderPass;
    const VkDevice device = m_context.device();
    // Attachment formats changed (HDR offscreen), so every existing
    // pipeline is incompatible: destroy each distinct handle once
    // and recreate the default pipeline against the scene pass.
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
    for (const auto& [key, pipeline] : m_pipelinesByState) {
        (void)key;
        destroyOnce(pipeline);
    }
    m_materialPipelines.clear();
    m_pipelinesByState.clear();
    m_pipeline = createPipeline(sceneRenderPass, PipelineStateDesc{}, "cube.vert.spv", "cube.frag.spv");
    m_pipelinesByState.emplace(pipelineKey(PipelineStateDesc{}, "cube.vert.spv", "cube.frag.spv"), m_pipeline);
    m_distinctPipelines = 1;
}

VkPipeline GraphicsPipeline::createSkyboxPipeline(VkRenderPass sceneRenderPass) {
    if (m_skyboxPipeline != VK_NULL_HANDLE) {
        return m_skyboxPipeline;
    }
    const VkDevice device = m_context.device();

    VkShaderModule vertexShader = createShaderModule("skybox.vert.spv");
    VkShaderModule fragmentShader = createShaderModule("skybox.frag.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertexShader;
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragmentShader;
    shaderStages[1].pName = "main";

    const VkVertexInputBindingDescription bindingDescription = Vertex::bindingDescription();
    const auto attributeDescriptions = Vertex::attributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDescription;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInput.pVertexAttributeDescriptions = attributeDescriptions.data();

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

    // Inside-out cube: front faces cull, so only the far inside of
    // the sky cube rasterizes; depth is written by the real scene.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;
    rasterizer.frontFace = toVulkan(kEngineFrontFace);

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Skybox vertices sit at the far plane (z = w trick), so the
    // depth test is LessOrEqual against the cleared 1.0 and nothing
    // is written back.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

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
    pipelineInfo.renderPass = sceneRenderPass;
    pipelineInfo.subpass = 0;

    const VkResult result =
        vkCreateGraphicsPipelines(device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_skyboxPipeline);
    vkDestroyShaderModule(device, vertexShader, nullptr);
    vkDestroyShaderModule(device, fragmentShader, nullptr);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("GraphicsPipeline: failed to create skybox pipeline");
    }
    return m_skyboxPipeline;
}

VkShaderModule GraphicsPipeline::createShaderModule(const char* fileName) const {
    const std::string path = std::string(ENGINE_SHADER_DIR) + "/" + fileName;
    // Async preload path: bytes a JobSystem worker already read cost
    // no main-thread disk I/O here. The fallback read keeps every
    // synchronous caller (shadow/skybox pipelines, tools) working.
    std::vector<char> code;
    {
        std::lock_guard lock(m_shaderCache->mutex);
        if (const auto found = m_shaderCache->bytes.find(fileName); found != m_shaderCache->bytes.end()) {
            code = found->second;
        }
    }
    if (code.empty()) {
        code = helpers::readFile(path);
    }

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

VkPipeline GraphicsPipeline::createPipeline(VkRenderPass renderPass, const PipelineStateDesc& state,
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
    // Front faces arrive counter-clockwise in framebuffer space:
    // the projection's Y flip and the positive-height viewport
    // cancel, so the shared engine convention (kEngineFrontFace)
    // keeps the visible faces of CCW-authored geometry and culls
    // its back faces. (The old CLOCKWISE declaration culled the
    // front faces instead -- meshes rendered inside-out.)
    rasterizer.frontFace = toVulkan(kEngineFrontFace);
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
    // Per-entity model matrices and material color/lighting arrive as push
    // constants (112 bytes, inside the guaranteed 128-byte minimum). The
    // default pipeline state is a solid material: depth test + depth write
    // enabled with Less comparison and back-face culling, so nearer solid
    // geometry always occludes what is behind it.
    // 112 bytes of draw data + a 4-byte cascade index at offset 112
    // (shadow-pass only), still inside the guaranteed 128-byte minimum.
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(DrawPushConstants) + sizeof(uint32_t);

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
    pipelineInfo.renderPass = renderPass;
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
