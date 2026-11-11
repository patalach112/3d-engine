#include "Graphics/Renderer.h"

#include "Core/Logger.h"
#include "Geometry/Cube.h"
#include "Graphics/EnvironmentMap.h"
#include "Graphics/GraphicsPipeline.h"
#include "Graphics/ShadowMap.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/VulkanHelpers.h"
#include "Platform/Window.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace engine {
namespace {

// Owned-image creation with cube/mip/layer control (the shared
// helpers only cover flat 2D images).
void createOwnedImage(VulkanContext& context, uint32_t width, uint32_t height, VkFormat format,
                      VkImageUsageFlags usage, bool cubeCompatible, uint32_t arrayLayers, uint32_t mipLevels,
                      VkImage& outImage, VkDeviceMemory& outMemory) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = cubeCompatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(context.device(), &imageInfo, nullptr, &outImage) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create image");
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context.device(), outImage, &requirements);
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        context.findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(context.device(), &allocateInfo, nullptr, &outMemory) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to allocate image memory");
    }
    vkBindImageMemory(context.device(), outImage, outMemory, 0);
}

VkImageView createTypedView(VulkanContext& context, VkImage image, VkFormat format, VkImageViewType viewType,
                            uint32_t mipLevels, uint32_t arrayLayers) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = viewType;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = arrayLayers;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(context.device(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create image view");
    }
    return view;
}

// One-shot upload: staging buffer -> TRANSFER_DST -> SHADER_READ_ONLY.
void uploadImageData(VulkanContext& context, VkImage image, VkDeviceSize byteSize, const void* data,
                     const std::vector<VkBufferImageCopy>& regions, uint32_t mipLevels, uint32_t arrayLayers) {
    Buffer staging(context, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.map();
    std::memcpy(staging.mappedData(), data, static_cast<size_t>(byteSize));
    staging.unmap();

    VkCommandBuffer commandBuffer = helpers::beginSingleTimeCommands(context);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = arrayLayers;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    vkCmdCopyBufferToImage(commandBuffer, staging.handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
    helpers::endSingleTimeCommands(context, commandBuffer);
}

VkShaderModule createPostShaderModule(VulkanContext& context, const char* fileName) {
    const std::string path = std::string(ENGINE_SHADER_DIR) + "/" + fileName;
    const std::vector<char> code = helpers::readFile(path);
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(context.device(), &createInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create post shader module from " + path);
    }
    return module;
}

// Fullscreen-triangle pipeline (vertex input comes from gl_VertexIndex).
VkPipeline createFullscreenPipeline(VulkanContext& context, VkRenderPass renderPass, VkPipelineLayout layout,
                                    const char* fragmentFile, bool additiveBlend) {
    const VkDevice device = context.device();
    VkShaderModule vertexShader = createPostShaderModule(context, "post.vert.spv");
    VkShaderModule fragmentShader = createPostShaderModule(context, fragmentFile);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // inert without culling; kept on-convention

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = additiveBlend ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(device, vertexShader, nullptr);
    vkDestroyShaderModule(device, fragmentShader, nullptr);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create fullscreen post pipeline");
    }
    return pipeline;
}

void destroyImageTriple(VkDevice device, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
}

} // namespace

Renderer::Renderer(VulkanContext& context, Window& window, Swapchain& swapchain, GraphicsPipeline& pipeline,
                   uint32_t maxFramesInFlight)
    : m_context(context), m_window(window), m_swapchain(swapchain), m_pipeline(pipeline),
      m_hud(context, swapchain), m_ui(context, swapchain),
      m_framesInFlight(std::clamp(maxFramesInFlight, 1u, static_cast<uint32_t>(kMaxFramesInFlight))) {
    // Startup event pumping: this constructor (render passes,
    // environment resources, pipeline creation with shader
    // compilation) is the longest blocking stretch of engine
    // initialization. Poll OS events between stages so the freshly
    // created window never sits long enough to be painted white /
    // marked "Not Responding" by the OS.
    m_window.pollEvents();
    createVertexBuffer();
    createIndexBuffer();
    createShadowResources();
    m_window.pollEvents();
    createSceneRenderPass();
    m_pipeline.setSceneRenderPass(m_sceneRenderPass);
    m_pipeline.createSkyboxPipeline(m_sceneRenderPass);
    m_window.pollEvents();
    createHdrTargets();
    createEnvironmentResources();
    m_window.pollEvents();
    createUniformBuffers();
    createDescriptorPool();
    createDescriptorSets();
    createPostResources();
    m_window.pollEvents();
    createCommandBuffers();
    createSyncObjects();
    m_window.pollEvents();
}

Renderer::~Renderer() {
    const VkDevice device = m_context.device();

    for (const VkSemaphore semaphore : m_renderFinishedSemaphores) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (const VkSemaphore semaphore : m_imageAvailableSemaphores) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (const VkFence fence : m_inFlightFences) {
        vkDestroyFence(device, fence, nullptr);
    }
    if (!m_commandBuffers.empty()) {
        vkFreeCommandBuffers(device, m_context.commandPool(),
                             static_cast<uint32_t>(m_commandBuffers.size()), m_commandBuffers.data());
    }
    destroyPostResources();
    destroyHdrTargets();
    if (m_sceneRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_sceneRenderPass, nullptr);
    }
    destroyImageTriple(device, m_environmentCube.image, m_environmentCube.memory, m_environmentCube.view);
    destroyImageTriple(device, m_irradianceCube.image, m_irradianceCube.memory, m_irradianceCube.view);
    destroyImageTriple(device, m_prefilterCube.image, m_prefilterCube.memory, m_prefilterCube.view);
    destroyImageTriple(device, m_brdfLut.image, m_brdfLut.memory, m_brdfLut.view);
    destroyImageTriple(device, m_whiteTexture.image, m_whiteTexture.memory, m_whiteTexture.view);
    for (GpuImage& texture : m_materialTextures) {
        destroyImageTriple(device, texture.image, texture.memory, texture.view);
    }
    if (m_environmentSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_environmentSampler, nullptr);
    }
    if (m_materialSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_materialSampler, nullptr);
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    }
    const auto destroyShadowTarget = [device](const ShadowTarget& target) {
        if (target.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, target.framebuffer, nullptr);
        }
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, target.view, nullptr);
        }
        if (target.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, target.image, nullptr);
        }
        if (target.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, target.memory, nullptr);
        }
    };
    for (const ShadowTarget& target : m_shadowTargets) {
        destroyShadowTarget(target);
    }
    destroyShadowTarget(m_spotShadowTarget);
    if (m_shadowRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_shadowRenderPass, nullptr);
    }
    if (m_shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_shadowSampler, nullptr);
    }
    // m_vertexBuffer / m_indexBuffer / m_uniformBuffers clean themselves up.
}

void Renderer::createShadowResources() {
    const VkDevice device = m_context.device();
    constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

    // One depth target per cascade, all sharing the render pass and
    // sampler; sampled as a sampler2D array in the main pass.
    for (ShadowTarget& target : m_shadowTargets) {
        helpers::createImage(m_context, kShadowMapSize, kShadowMapSize, kShadowFormat, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, target.image, target.memory);
        target.view = helpers::createImageView(m_context, target.image, kShadowFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    // Phase 32: the spot shadow target (smaller — one light, one
    // perspective slice) shares the depth format and sampler.
    helpers::createImage(m_context, kSpotShadowMapSize, kSpotShadowMapSize, kShadowFormat,
                         VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_spotShadowTarget.image, m_spotShadowTarget.memory);
    m_spotShadowTarget.view =
        helpers::createImageView(m_context, m_spotShadowTarget.image, kShadowFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Plain (non-comparison) sampler: the fragment shader fetches depth
    // and compares manually, which enables the 3x3 PCF kernel. Border is
    // opaque white (depth 1.0 = fully lit), so fragments outside the
    // light frustum can never darken.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create shadow map sampler");
    }

    // Depth-only render pass. The attachment starts undefined and ends in
    // SHADER_READ_ONLY_OPTIMAL; the two subpass dependencies order the
    // main pass's texture reads after this pass's depth writes (and this
    // pass's writes after the previous frame's reads).
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = kShadowFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthReference{};
    depthReference.attachment = 0;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthReference;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = 0;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = 0;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();
    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_shadowRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create shadow render pass");
    }

    for (ShadowTarget& target : m_shadowTargets) {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_shadowRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &target.view;
        framebufferInfo.width = kShadowMapSize;
        framebufferInfo.height = kShadowMapSize;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &target.framebuffer) != VK_SUCCESS) {
            throw std::runtime_error("Renderer: failed to create shadow framebuffer");
        }
    }

    {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_shadowRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_spotShadowTarget.view;
        framebufferInfo.width = kSpotShadowMapSize;
        framebufferInfo.height = kSpotShadowMapSize;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &m_spotShadowTarget.framebuffer) != VK_SUCCESS) {
            throw std::runtime_error("Renderer: failed to create spot shadow framebuffer");
        }
    }

    m_pipeline.createShadowPipeline(m_shadowRenderPass);
}

void Renderer::createSceneRenderPass() {
    // HDR offscreen scene pass: linear RGBA16F color (read later by
    // bloom + composite) plus depth. Ends in SHADER_READ_ONLY so the
    // post chain can sample the color directly.
    constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = kHdrFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorReference{};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthReference{};
    depthReference.attachment = 1;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    const std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();
    if (vkCreateRenderPass(m_context.device(), &renderPassInfo, nullptr, &m_sceneRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create HDR scene render pass");
    }
}

void Renderer::createHdrTargets() {
    const VkExtent2D extent = m_swapchain.extent();
    const VkDevice device = m_context.device();
    destroyHdrTargets();
    m_hdrTargets.resize(m_framesInFlight);
    for (HdrTarget& target : m_hdrTargets) {
        createOwnedImage(m_context, extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, 1, 1,
                         target.color.image, target.color.memory);
        target.color.view = createTypedView(m_context, target.color.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                            VK_IMAGE_VIEW_TYPE_2D, 1, 1);
        createOwnedImage(m_context, extent.width, extent.height, VK_FORMAT_D32_SFLOAT,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, false, 1, 1, target.depth.image,
                         target.depth.memory);
        // Depth view needs the depth aspect: build it inline (the
        // shared typed-view helper is color-only).
        VkImageViewCreateInfo depthViewInfo{};
        depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image = target.depth.image;
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthViewInfo.subresourceRange.levelCount = 1;
        depthViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &depthViewInfo, nullptr, &target.depth.view) != VK_SUCCESS) {
            throw std::runtime_error("Renderer: failed to create HDR depth view");
        }
        const std::array<VkImageView, 2> attachments = {target.color.view, target.depth.view};
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_sceneRenderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &target.framebuffer) != VK_SUCCESS) {
            throw std::runtime_error("Renderer: failed to create HDR framebuffer");
        }
    }
}

void Renderer::destroyHdrTargets() {
    const VkDevice device = m_context.device();
    for (HdrTarget& target : m_hdrTargets) {
        if (target.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, target.framebuffer, nullptr);
            target.framebuffer = VK_NULL_HANDLE;
        }
        destroyImageTriple(device, target.color.image, target.color.memory, target.color.view);
        destroyImageTriple(device, target.depth.image, target.depth.memory, target.depth.view);
    }
    m_hdrTargets.clear();
}

void Renderer::createEnvironmentResources() {
    // Deterministic CPU generation (Graphics/EnvironmentMap), then
    // one staging upload per resource. Sizes are small: the sky is a
    // smooth gradient, so 64px faces prefilter beautifully.
    constexpr uint32_t kEnvironmentSize = 64;
    constexpr uint32_t kIrradianceSize = 32;
    constexpr uint32_t kLutSize = 256;
    constexpr VkFormat kCubeFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

    const CubeMapData environment = generateEnvironmentCube(kEnvironmentSize, directionalLightDirection());
    const CubeMapData irradiance = generateIrradiance(environment, kIrradianceSize, 512);
    const std::vector<CubeMapData> prefiltered =
        generatePrefiltered(environment, kEnvironmentSize, kPrefilterLevels, 256);
    const std::vector<glm::vec2> lut = integrateBrdfLut(kLutSize, 1024);

    const auto uploadCube = [this](const std::vector<const CubeMapData*>& levels, GpuImage& out) {
        uint32_t totalTexels = 0;
        for (const CubeMapData* level : levels) {
            totalTexels += 6 * level->size * level->size;
        }
        std::vector<glm::vec4> pixels;
        pixels.reserve(totalTexels);
        std::vector<VkBufferImageCopy> regions;
        VkDeviceSize offset = 0;
        for (uint32_t levelIndex = 0; levelIndex < levels.size(); ++levelIndex) {
            const CubeMapData& level = *levels[levelIndex];
            for (uint32_t face = 0; face < 6; ++face) {
                for (const glm::vec3& texelValue : level.faces[face]) {
                    pixels.emplace_back(texelValue, 1.0f);
                }
                VkBufferImageCopy region{};
                region.bufferOffset = offset;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = levelIndex;
                region.imageSubresource.baseArrayLayer = face;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {level.size, level.size, 1};
                regions.push_back(region);
                offset += static_cast<VkDeviceSize>(level.size) * level.size * sizeof(glm::vec4);
            }
        }
        const uint32_t baseSize = levels.front()->size;
        createOwnedImage(m_context, baseSize, baseSize, kCubeFormat,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, 6,
                         static_cast<uint32_t>(levels.size()), out.image, out.memory);
        out.view = createTypedView(m_context, out.image, kCubeFormat, VK_IMAGE_VIEW_TYPE_CUBE,
                                   static_cast<uint32_t>(levels.size()), 6);
        uploadImageData(m_context, out.image, offset, pixels.data(), regions,
                        static_cast<uint32_t>(levels.size()), 6);
    };

    uploadCube({&environment}, m_environmentCube);
    uploadCube({&irradiance}, m_irradianceCube);
    std::vector<const CubeMapData*> prefilterLevels;
    for (const CubeMapData& level : prefiltered) {
        prefilterLevels.push_back(&level);
    }
    uploadCube(prefilterLevels, m_prefilterCube);

    // BRDF integration LUT (RG float, NoV x roughness).
    createOwnedImage(m_context, kLutSize, kLutSize, VK_FORMAT_R32G32_SFLOAT,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, 1, 1, m_brdfLut.image,
                     m_brdfLut.memory);
    m_brdfLut.view =
        createTypedView(m_context, m_brdfLut.image, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
    VkBufferImageCopy lutRegion{};
    lutRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    lutRegion.imageSubresource.layerCount = 1;
    lutRegion.imageExtent = {kLutSize, kLutSize, 1};
    uploadImageData(m_context, m_brdfLut.image, static_cast<VkDeviceSize>(lut.size()) * sizeof(glm::vec2),
                    lut.data(), {lutRegion}, 1, 1);

    // Shared samplers: environment sampling wants trilinear cube
    // filtering across prefilter LODs; material textures are
    // single-mip and clamped.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(kPrefilterLevels - 1);
    if (vkCreateSampler(m_context.device(), &samplerInfo, nullptr, &m_environmentSampler) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create environment sampler");
    }
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(m_context.device(), &samplerInfo, nullptr, &m_materialSampler) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create material sampler");
    }

    // 1x1 white fallback bound into every untextured material slot.
    const std::array<uint8_t, 4> whitePixel = {255, 255, 255, 255};
    createOwnedImage(m_context, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, 1, 1,
                     m_whiteTexture.image, m_whiteTexture.memory);
    m_whiteTexture.view =
        createTypedView(m_context, m_whiteTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
    VkBufferImageCopy whiteRegion{};
    whiteRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    whiteRegion.imageSubresource.layerCount = 1;
    whiteRegion.imageExtent = {1, 1, 1};
    uploadImageData(m_context, m_whiteTexture.image, whitePixel.size(), whitePixel.data(), {whiteRegion}, 1, 1);
}

void Renderer::syncMaterialTextures(std::span<const MaterialTextureUpload> textures, uint64_t registryVersion) {
    if (registryVersion == m_syncedTextureVersion) {
        return;
    }
    m_syncedTextureVersion = registryVersion;
    // Textures may be sampled by in-flight frames; rebuilds are rare
    // (registration / hot reload), so drain the device first.
    m_context.waitIdle();
    const VkDevice device = m_context.device();
    for (GpuImage& texture : m_materialTextures) {
        destroyImageTriple(device, texture.image, texture.memory, texture.view);
    }
    m_materialTextures.clear();

    const size_t count = std::min(textures.size(), static_cast<size_t>(kMaxMaterialTextures));
    m_materialTextures.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const MaterialTextureUpload& upload = textures[i];
        GpuImage texture;
        if (upload.width > 0 && upload.height > 0 &&
            upload.rgba.size() == static_cast<size_t>(upload.width) * upload.height * 4) {
            createOwnedImage(m_context, upload.width, upload.height, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, 1, 1,
                             texture.image, texture.memory);
            texture.view = createTypedView(m_context, texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_VIEW_TYPE_2D, 1, 1);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {upload.width, upload.height, 1};
            uploadImageData(m_context, texture.image, upload.rgba.size(), upload.rgba.data(), {region}, 1, 1);
        }
        m_materialTextures.push_back(texture);
    }

    // Repoint binding 2 of every frame's descriptor set.
    for (uint32_t frame = 0; frame < m_framesInFlight; ++frame) {
        std::array<VkDescriptorImageInfo, kMaxMaterialTextures> infos{};
        for (uint32_t slot = 0; slot < kMaxMaterialTextures; ++slot) {
            infos[slot].sampler = m_materialSampler;
            infos[slot].imageView = slot < m_materialTextures.size() &&
                                            m_materialTextures[slot].view != VK_NULL_HANDLE
                                        ? m_materialTextures[slot].view
                                        : m_whiteTexture.view;
            infos[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSets[frame];
        write.dstBinding = 2;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = kMaxMaterialTextures;
        write.pImageInfo = infos.data();
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

void Renderer::createPostResources() {
    const VkDevice device = m_context.device();

    // Two sampled inputs per post step (bloom passes use binding 0;
    // the composite uses both). One layout shared by all three
    // fullscreen pipelines.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_postDescriptorLayout) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create post descriptor layout");
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 4;
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_postDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_postPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create post pipeline layout");
    }

    // Bloom pass: single HDR color attachment, preserved on entry
    // (the upsample adds onto the downsampled level), sampled after.
    VkAttachmentDescription bloomAttachment{};
    bloomAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    bloomAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    bloomAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    bloomAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    bloomAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    bloomAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    bloomAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bloomAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference bloomReference{};
    bloomReference.attachment = 0;
    bloomReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &bloomReference;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo bloomPassInfo{};
    bloomPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    bloomPassInfo.attachmentCount = 1;
    bloomPassInfo.pAttachments = &bloomAttachment;
    bloomPassInfo.subpassCount = 1;
    bloomPassInfo.pSubpasses = &subpass;
    bloomPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    bloomPassInfo.pDependencies = dependencies.data();
    if (vkCreateRenderPass(device, &bloomPassInfo, nullptr, &m_bloomRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create bloom render pass");
    }

    m_bloomDownPipeline =
        createFullscreenPipeline(m_context, m_bloomRenderPass, m_postPipelineLayout, "bloomDown.frag.spv", false);
    m_bloomUpPipeline =
        createFullscreenPipeline(m_context, m_bloomRenderPass, m_postPipelineLayout, "bloomUp.frag.spv", true);
    m_compositePipeline = createFullscreenPipeline(m_context, m_swapchain.renderPass(), m_postPipelineLayout,
                                                  "composite.frag.spv", false);

    // Descriptor sets: per frame, 5 down + 5 up + 1 composite.
    constexpr uint32_t kSetsPerFrame = 2 * kBloomLevels + 1;
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = m_framesInFlight * kSetsPerFrame * 2;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = m_framesInFlight * kSetsPerFrame;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_postDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create post descriptor pool");
    }

    const VkExtent2D extent = m_swapchain.extent();
    m_bloomChains.resize(m_framesInFlight);
    for (BloomChain& chain : m_bloomChains) {
        uint32_t width = extent.width;
        uint32_t height = extent.height;
        for (uint32_t level = 0; level < kBloomLevels; ++level) {
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
            BloomLevel& target = chain.levels[level];
            target.extent = {width, height};
            createOwnedImage(m_context, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, 1, 1,
                             target.target.image, target.target.memory);
            target.target.view = createTypedView(m_context, target.target.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                 VK_IMAGE_VIEW_TYPE_2D, 1, 1);
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = m_bloomRenderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &target.target.view;
            framebufferInfo.width = width;
            framebufferInfo.height = height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &target.framebuffer) != VK_SUCCESS) {
                throw std::runtime_error("Renderer: failed to create bloom framebuffer");
            }
        }
        std::vector<VkDescriptorSetLayout> layouts;
        layouts.reserve(kSetsPerFrame);
        for (uint32_t i = 0; i < kSetsPerFrame; ++i) {
            layouts.push_back(m_postDescriptorLayout);
        }
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = m_postDescriptorPool;
        allocateInfo.descriptorSetCount = kSetsPerFrame;
        allocateInfo.pSetLayouts = layouts.data();
        std::vector<VkDescriptorSet> sets(kSetsPerFrame);
        if (vkAllocateDescriptorSets(device, &allocateInfo, sets.data()) != VK_SUCCESS) {
            throw std::runtime_error("Renderer: failed to allocate post descriptor sets");
        }
        for (uint32_t level = 0; level < kBloomLevels; ++level) {
            chain.downSets[level] = sets[level];
            chain.upSets[level] = sets[kBloomLevels + level];
        }
        chain.compositeSet = sets[2 * kBloomLevels];
    }
    writePostDescriptorSets();
}

void Renderer::writePostDescriptorSets() {
    const VkDevice device = m_context.device();
    for (uint32_t frame = 0; frame < m_framesInFlight; ++frame) {
        BloomChain& chain = m_bloomChains[frame];
        const auto writeSet = [this, device](VkDescriptorSet set, VkImageView input, VkImageView second) {
            std::array<VkDescriptorImageInfo, 2> infos{};
            infos[0].sampler = m_environmentSampler;
            infos[0].imageView = input;
            infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[1].sampler = m_environmentSampler;
            infos[1].imageView = second;
            infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            std::array<VkWriteDescriptorSet, 2> writes{};
            for (uint32_t binding = 0; binding < 2; ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = set;
                writes[binding].dstBinding = binding;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[binding].descriptorCount = 1;
                writes[binding].pImageInfo = &infos[binding];
            }
            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        };
        for (uint32_t level = 0; level < kBloomLevels; ++level) {
            const VkImageView downInput =
                level == 0 ? m_hdrTargets[frame].color.view : chain.levels[level - 1].target.view;
            writeSet(chain.downSets[level], downInput, downInput);
            const VkImageView upInput = chain.levels[std::min(level + 1, kBloomLevels - 1)].target.view;
            writeSet(chain.upSets[level], upInput, upInput);
        }
        writeSet(chain.compositeSet, m_hdrTargets[frame].color.view, chain.levels[0].target.view);
    }
}

void Renderer::destroyPostResources() {
    const VkDevice device = m_context.device();
    for (BloomChain& chain : m_bloomChains) {
        for (BloomLevel& level : chain.levels) {
            if (level.framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, level.framebuffer, nullptr);
                level.framebuffer = VK_NULL_HANDLE;
            }
            destroyImageTriple(device, level.target.image, level.target.memory, level.target.view);
        }
    }
    m_bloomChains.clear();
    if (m_postDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_postDescriptorPool, nullptr);
        m_postDescriptorPool = VK_NULL_HANDLE;
    }
    if (m_bloomDownPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_bloomDownPipeline, nullptr);
        m_bloomDownPipeline = VK_NULL_HANDLE;
    }
    if (m_bloomUpPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_bloomUpPipeline, nullptr);
        m_bloomUpPipeline = VK_NULL_HANDLE;
    }
    if (m_compositePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_compositePipeline, nullptr);
        m_compositePipeline = VK_NULL_HANDLE;
    }
    if (m_postPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_postPipelineLayout, nullptr);
        m_postPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_postDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_postDescriptorLayout, nullptr);
        m_postDescriptorLayout = VK_NULL_HANDLE;
    }
    if (m_bloomRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_bloomRenderPass, nullptr);
        m_bloomRenderPass = VK_NULL_HANDLE;
    }
}

void Renderer::recordBloomChain(VkCommandBuffer commandBuffer) {
    if (!m_postSettings.bloomEnabled) {
        return;
    }
    BloomChain& chain = m_bloomChains[m_currentFrame];
    const auto beginLevel = [this, commandBuffer](const BloomLevel& level) {
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_bloomRenderPass;
        renderPassInfo.framebuffer = level.framebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = level.extent;
        renderPassInfo.clearValueCount = 0;
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{};
        viewport.width = static_cast<float>(level.extent.width);
        viewport.height = static_cast<float>(level.extent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.extent = level.extent;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    };

    struct PostPush {
        float texelX;
        float texelY;
        float threshold;
        float knee;
    };

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomDownPipeline);
    for (uint32_t level = 0; level < kBloomLevels; ++level) {
        const BloomLevel& target = chain.levels[level];
        beginLevel(target);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipelineLayout, 0, 1,
                                &chain.downSets[level], 0, nullptr);
        const VkExtent2D sourceExtent =
            level == 0 ? m_swapchain.extent() : chain.levels[level - 1].extent;
        const PostPush push{1.0f / static_cast<float>(std::max(1u, sourceExtent.width)),
                            1.0f / static_cast<float>(std::max(1u, sourceExtent.height)),
                            level == 0 ? m_postSettings.bloomThreshold : 0.0f, 0.5f};
        vkCmdPushConstants(commandBuffer, m_postPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPush),
                           &push);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer);
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomUpPipeline);
    for (uint32_t level = kBloomLevels - 1; level-- > 0;) {
        const BloomLevel& target = chain.levels[level];
        const BloomLevel& source = chain.levels[level + 1];
        beginLevel(target);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipelineLayout, 0, 1,
                                &chain.upSets[level], 0, nullptr);
        const PostPush push{1.0f / static_cast<float>(std::max(1u, source.extent.width)),
                            1.0f / static_cast<float>(std::max(1u, source.extent.height)), 0.0f, 0.0f};
        vkCmdPushConstants(commandBuffer, m_postPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPush),
                           &push);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer);
    }
}

SceneViewportRect resolveSceneViewport(const SceneViewportRect& requested, VkExtent2D extent) {
    if (requested.empty() || extent.width == 0 || extent.height == 0) {
        return SceneViewportRect{0, 0, extent.width, extent.height};
    }
    const int32_t maxX = static_cast<int32_t>(extent.width);
    const int32_t maxY = static_cast<int32_t>(extent.height);
    const int32_t x0 = std::clamp(requested.x, 0, maxX);
    const int32_t y0 = std::clamp(requested.y, 0, maxY);
    const int32_t x1 = static_cast<int32_t>(
        std::clamp(static_cast<int64_t>(requested.x) + requested.width, static_cast<int64_t>(x0),
                   static_cast<int64_t>(maxX)));
    const int32_t y1 = static_cast<int32_t>(
        std::clamp(static_cast<int64_t>(requested.y) + requested.height, static_cast<int64_t>(y0),
                   static_cast<int64_t>(maxY)));
    // Fully off-screen (or clamped away entirely): collapse to zero
    // size at the clamped origin. The scene simply shows nothing --
    // it must NEVER fall back to the full framebuffer, which would
    // splash the world across every neighbouring dock.
    return SceneViewportRect{x0, y0, static_cast<uint32_t>(std::max(0, x1 - x0)),
                             static_cast<uint32_t>(std::max(0, y1 - y0))};
}

SceneViewportRect Renderer::effectiveSceneViewport(VkExtent2D extent) const {
    return resolveSceneViewport(m_sceneViewport, extent);
}

void Renderer::setSceneViewport(SceneViewportRect viewport) {
    m_sceneViewport = viewport;
    // Diagnostics: a non-empty confinement request that resolution
    // had to alter (off-screen, oversized, clamped at an edge) means
    // the caller's layout disagrees with the framebuffer. Warn once
    // per distinct request value instead of rendering wrong quietly;
    // a steady valid rect -- the editor's normal state -- stays
    // silent. An empty request is the player's full-frame mode.
    if (viewport.empty() || viewport == m_confinementWarned) {
        return;
    }
    const VkExtent2D extent = m_swapchain.extent();
    if (extent.width == 0 || extent.height == 0) {
        return; // minimized / mid-recreate: nothing meaningful to check
    }
    const SceneViewportRect resolved = resolveSceneViewport(viewport, extent);
    if (resolved == viewport) {
        return;
    }
    m_confinementWarned = viewport;
    ENGINE_LOG_WARNING("Scene viewport (" << viewport.x << ", " << viewport.y << ", " << viewport.width << "x"
                                          << viewport.height << ") does not fit the " << extent.width << "x"
                                          << extent.height << " framebuffer; scene confinement clamped to ("
                                          << resolved.x << ", " << resolved.y << ", " << resolved.width << "x"
                                          << resolved.height << ") -- the scene stays inside the framebuffer.");
}

void Renderer::recordComposite(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    const VkExtent2D extent = m_swapchain.extent();

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {0.0f, 0.0f, 0.0f, 1.0f};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_swapchain.renderPass();
    renderPassInfo.framebuffer = m_swapchain.framebuffer(imageIndex);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // The composite (the only pass writing the swapchain image)
    // presents the tone-mapped scene strictly inside the viewport
    // rect. The render-pass clear above has already painted the
    // whole image with the editor background colour, so the docks
    // get a defined backdrop even before the UI overlay draws them.
    //
    // The confinement MUST be scissor-only. post.vert derives
    // fragUv from the NDC position, so narrowing the Vulkan
    // *viewport* to the scene rect would stretch the rasterized
    // triangle -- and with it the UVs -- across the panel, sampling
    // the whole HDR image there while the scene pass wrote the
    // world only into the panel's own texels. The world would then
    // render squashed and offset inside the panel, displaced from
    // where the camera projection, picking rays and gizmo
    // projection (all pure 1:1 mappings onto the rect) place it.
    // With a full-extent viewport the UVs are true framebuffer
    // coordinates: panel pixel (x, y) samples scene texel (x, y),
    // exactly where the scene pass put it, and the scissor alone
    // keeps composite writes out of the docks.
    const SceneViewportRect sceneRect = effectiveSceneViewport(extent);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset = {sceneRect.x, sceneRect.y};
    scissor.extent = {sceneRect.width, sceneRect.height};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);
    BloomChain& chain = m_bloomChains[m_currentFrame];
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipelineLayout, 0, 1,
                            &chain.compositeSet, 0, nullptr);
    struct CompositePush {
        float exposure;
        float bloomStrength;
        float bloomEnabled;
        float chromaticAberration;
    };
    const CompositePush push{m_postSettings.exposure, m_postSettings.bloomStrength,
                             m_postSettings.bloomEnabled ? 1.0f : 0.0f, m_postSettings.chromaticAberration};
    vkCmdPushConstants(commandBuffer, m_postPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CompositePush),
                       &push);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    // The UI overlay addresses the whole framebuffer in pixels:
    // widen the composite's scissor back to the full image (the
    // viewport never narrowed; UIRenderer additionally sets its
    // own per-segment scissors).
    VkViewport fullViewport{};
    fullViewport.width = static_cast<float>(extent.width);
    fullViewport.height = static_cast<float>(extent.height);
    fullViewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &fullViewport);
    VkRect2D fullScissor{};
    fullScissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &fullScissor);

    // Phase 26 UI overlay (analytic rounded-rect panels/widgets),
    // drawn on the presented image after the composite, under the
    // FPS HUD text. The frame slot keeps the overlay's staging
    // buffers disjoint from any still-in-flight submission.
    m_ui.record(commandBuffer, extent, m_currentFrame);

    // FPS HUD overlay, drawn on the presented image after the composite.
    m_hud.record(commandBuffer, extent);

    vkCmdEndRenderPass(commandBuffer);
}

void Renderer::createCommandBuffers() {
    m_commandBuffers.resize(m_framesInFlight);

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_context.commandPool();
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());

    if (vkAllocateCommandBuffers(m_context.device(), &allocateInfo, m_commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to allocate command buffers");
    }
}

void Renderer::createRenderFinishedSemaphores() {
    for (const VkSemaphore semaphore : m_renderFinishedSemaphores) {
        vkDestroySemaphore(m_context.device(), semaphore, nullptr);
    }
    m_renderFinishedSemaphores.assign(m_swapchain.imageCount(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore& semaphore : m_renderFinishedSemaphores) {
        if (vkCreateSemaphore(m_context.device(), &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
            throw std::runtime_error("Renderer: failed to create render-finished semaphore");
        }
    }
}

void Renderer::createSyncObjects() {
    m_imageAvailableSemaphores.resize(m_framesInFlight);
    m_inFlightFences.resize(m_framesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first frame must not deadlock

    const VkDevice device = m_context.device();
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Renderer: failed to create synchronization objects");
        }
    }

    createRenderFinishedSemaphores();
}

void Renderer::createVertexBuffer() {
    const std::vector<Vertex> vertices = Cube::vertices();
    const VkDeviceSize size = sizeof(Vertex) * vertices.size();
    m_vertexBuffer = createDeviceLocalBuffer(m_context, vertices.data(), size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

void Renderer::createIndexBuffer() {
    const std::vector<uint32_t> indices = Cube::indices();
    m_indexCount = static_cast<uint32_t>(indices.size());
    const VkDeviceSize size = sizeof(uint32_t) * indices.size();
    m_indexBuffer = createDeviceLocalBuffer(m_context, indices.data(), size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void Renderer::createUniformBuffers() {
    constexpr VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    m_uniformBuffers.reserve(m_framesInFlight);
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        Buffer buffer(m_context, bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        buffer.map(); // persistently mapped; rewritten each frame
        m_uniformBuffers.push_back(std::move(buffer));
    }
}

void Renderer::createDescriptorPool() {
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = m_framesInFlight;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // shadow array (4) + material textures (32) + irradiance/prefilter/LUT/environment (4) + spot shadow (1)
    poolSizes[1].descriptorCount = m_framesInFlight * (kMaxShadowCascades + kMaxMaterialTextures + 4 + 1);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = m_framesInFlight;

    if (vkCreateDescriptorPool(m_context.device(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create descriptor pool");
    }
}

void Renderer::createDescriptorSets() {
    const std::vector<VkDescriptorSetLayout> layouts(m_framesInFlight, m_pipeline.descriptorSetLayout());

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();

    m_descriptorSets.resize(m_framesInFlight);
    if (vkAllocateDescriptorSets(m_context.device(), &allocateInfo, m_descriptorSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to allocate descriptor sets");
    }

    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[i].handle();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        std::array<VkDescriptorImageInfo, kMaxShadowCascades> shadowInfos{};
        for (uint32_t cascade = 0; cascade < kMaxShadowCascades; ++cascade) {
            shadowInfos[cascade].sampler = m_shadowSampler;
            shadowInfos[cascade].imageView = m_shadowTargets[cascade].view;
            shadowInfos[cascade].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        std::array<VkDescriptorImageInfo, kMaxMaterialTextures> materialInfos{};
        for (uint32_t slot = 0; slot < kMaxMaterialTextures; ++slot) {
            materialInfos[slot].sampler = m_materialSampler;
            materialInfos[slot].imageView = m_whiteTexture.view;
            materialInfos[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        VkDescriptorImageInfo irradianceInfo{};
        irradianceInfo.sampler = m_environmentSampler;
        irradianceInfo.imageView = m_irradianceCube.view;
        irradianceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo prefilterInfo{};
        prefilterInfo.sampler = m_environmentSampler;
        prefilterInfo.imageView = m_prefilterCube.view;
        prefilterInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo lutInfo{};
        lutInfo.sampler = m_environmentSampler;
        lutInfo.imageView = m_brdfLut.view;
        lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo environmentInfo{};
        environmentInfo.sampler = m_environmentSampler;
        environmentInfo.imageView = m_environmentCube.view;
        environmentInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo spotShadowInfo{};
        spotShadowInfo.sampler = m_shadowSampler;
        spotShadowInfo.imageView = m_spotShadowTarget.view;
        spotShadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 8> writes{};
        const auto bufferWrite = [&writes](uint32_t index, VkDescriptorSet set, const VkDescriptorBufferInfo* info) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = set;
            writes[index].dstBinding = 0;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[index].descriptorCount = 1;
            writes[index].pBufferInfo = info;
        };
        const auto imageWrite = [&writes](uint32_t index, VkDescriptorSet set, uint32_t binding, uint32_t count,
                                          const VkDescriptorImageInfo* infos) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = set;
            writes[index].dstBinding = binding;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index].descriptorCount = count;
            writes[index].pImageInfo = infos;
        };
        bufferWrite(0, m_descriptorSets[i], &bufferInfo);
        imageWrite(1, m_descriptorSets[i], 1, kMaxShadowCascades, shadowInfos.data());
        imageWrite(2, m_descriptorSets[i], 2, kMaxMaterialTextures, materialInfos.data());
        imageWrite(3, m_descriptorSets[i], 3, 1, &irradianceInfo);
        imageWrite(4, m_descriptorSets[i], 4, 1, &prefilterInfo);
        imageWrite(5, m_descriptorSets[i], 5, 1, &lutInfo);
        imageWrite(6, m_descriptorSets[i], 6, 1, &environmentInfo);
        imageWrite(7, m_descriptorSets[i], 7, 1, &spotShadowInfo);

        vkUpdateDescriptorSets(m_context.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    }
}

void Renderer::updateUniformBuffer(const glm::mat4& view, const glm::mat4& projection,
                                   const FrameLighting& lighting) {
    UniformBufferObject ubo{};
    ubo.view = view;
    ubo.proj = projection;
    static const glm::mat4 kLegacyCascade =
        directionalLightViewProjection(directionalLightDirection(), shadowSceneCenter(), kShadowSceneExtent,
                                       kShadowLightDistance, kShadowNearPlane, kShadowFarPlane);
    for (uint32_t i = 0; i < kMaxShadowCascades; ++i) {
        ubo.lightViewProj[i] = lighting.cascadesFilled ? lighting.cascades[i] : kLegacyCascade;
    }
    ubo.lightDirection = glm::vec4{glm::normalize(lighting.directionalDirection), lighting.directionalRadiance};
    ubo.environmentParams = glm::vec4{m_postSettings.environmentIntensity,
                                      static_cast<float>(kPrefilterLevels - 1),
                                      m_postSettings.skyboxEnabled ? 1.0f : 0.0f, 0.0f};
    ubo.cascadeSplits = lighting.cascadeSplits;
    ubo.lightCounts = lighting.lightCounts;
    ubo.lightPositionRange = lighting.lightPositionRange;
    ubo.lightColorIntensity = lighting.lightColorIntensity;
    ubo.lightDirectionType = lighting.lightDirectionType;
    ubo.lightCone = lighting.lightCone;
    ubo.directionalColor = glm::vec4{lighting.directionalColor, lighting.directionalCastShadow ? 1.0f : 0.0f};
    ubo.spotShadowViewProj = lighting.spotShadowViewProj;
    ubo.spotShadowParams = glm::vec4{lighting.spotShadowEnabled ? static_cast<float>(lighting.spotShadowLightIndex)
                                                                 : -1.0f,
                                     0.0f, 0.0f, 0.0f};
    void* mapped = m_uniformBuffers[m_currentFrame].mappedData();
    std::memcpy(mapped, &ubo, sizeof(ubo));
}

void Renderer::recordShadowCascades(VkCommandBuffer commandBuffer, std::span<const DrawItem> drawItems,
                                    const std::array<glm::mat4, kMaxShadowCascades>& cascadeMatrices,
                                    uint32_t cascadeCount) {
    // One depth-only pass per cascade, each into its own target. The
    // shadow vertex shader transforms vertices with the cascade's
    // light matrix (selected by the pushed cascade index); the main
    // pass samples the matching slice per fragment.
    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.shadowPipeline());

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(kShadowMapSize);
    viewport.height = static_cast<float>(kShadowMapSize);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {kShadowMapSize, kShadowMapSize};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const VkBuffer vertexBuffers[] = {m_vertexBuffer.handle()};
    constexpr VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.layout(), 0, 1,
                            &m_descriptorSets[m_currentFrame], 0, nullptr);

    const uint32_t passes = std::max(1u, std::min(cascadeCount, kMaxShadowCascades));
    for (uint32_t cascade = 0; cascade < passes; ++cascade) {
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_shadowRenderPass;
        renderPassInfo.framebuffer = m_shadowTargets[cascade].framebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {kShadowMapSize, kShadowMapSize};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearValue;
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        for (const DrawItem& item : drawItems) {
            // 112-byte draw payload (model matrix at its head) plus
            // the cascade index at offset 112.
            struct ShadowPush {
                DrawPushConstants draw;
                uint32_t cascadeIndex;
            };
            const glm::ivec4 textureIndices{item.textureIndices[0], item.textureIndices[1],
                                            item.textureIndices[2], item.textureIndices[3]};
            const ShadowPush push{{item.model, item.baseColor, item.pbrParams, textureIndices}, cascade};
            vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(ShadowPush), &push);
            vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(commandBuffer);
    }
    (void)cascadeMatrices; // matrices live in the UBO; the index selects
}

void Renderer::recordSpotShadow(VkCommandBuffer commandBuffer, std::span<const DrawItem> drawItems) {
    // One depth-only pass from the spot's point of view into its
    // own target. Same pipeline and push block as the cascades;
    // the push index kMaxShadowCascades selects the spot matrix
    // (UBO.spotShadowViewProj) in shadow.vert.
    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.shadowPipeline());

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(kSpotShadowMapSize);
    viewport.height = static_cast<float>(kSpotShadowMapSize);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {kSpotShadowMapSize, kSpotShadowMapSize};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const VkBuffer vertexBuffers[] = {m_vertexBuffer.handle()};
    constexpr VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.layout(), 0, 1,
                            &m_descriptorSets[m_currentFrame], 0, nullptr);

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_shadowRenderPass;
    renderPassInfo.framebuffer = m_spotShadowTarget.framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {kSpotShadowMapSize, kSpotShadowMapSize};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    for (const DrawItem& item : drawItems) {
        struct ShadowPush {
            DrawPushConstants draw;
            uint32_t cascadeIndex;
        };
        const glm::ivec4 textureIndices{item.textureIndices[0], item.textureIndices[1], item.textureIndices[2],
                                        item.textureIndices[3]};
        const ShadowPush push{{item.model, item.baseColor, item.pbrParams, textureIndices}, kMaxShadowCascades};
        vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPush),
                           &push);
        vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);
}

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                                   std::span<const DrawItem> drawItems,
                                   const std::array<glm::mat4, kMaxShadowCascades>& cascadeMatrices,
                                   uint32_t cascadeCount, bool spotShadowEnabled) {
    // Mirror any glyph pages the UI batch rasterized while it was
    // being built (new glyphs/sizes) into their GPU images. This
    // must stay outside every render pass: uploads transition
    // image layouts and wait for completion. No-op on frames that
    // rasterized nothing.
    m_ui.syncFontTextures();

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to begin recording command buffer");
    }

    // Shadow cascades first: they fill the depth textures the main pass samples.
    recordShadowCascades(commandBuffer, drawItems, cascadeMatrices, cascadeCount);
    if (spotShadowEnabled) {
        recordSpotShadow(commandBuffer, drawItems);
    }

    const VkExtent2D extent = m_swapchain.extent();

    // --- HDR scene pass (offscreen): skybox, then material draws ---
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {0.05f, 0.05f, 0.08f, 1.0f}; // behind the skybox
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_sceneRenderPass;
    renderPassInfo.framebuffer = m_hdrTargets[m_currentFrame].framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // The scene rasterizes only inside the editor's viewport rect
    // (full extent for the standalone player): the render area above
    // still spans the target so the clear defines every texel the
    // bloom chain may sample, but viewport + scissor clip all draws.
    // (A fully off-screen request resolves to zero size; the viewport
    // gets a harmless 1px floor so no driver ever sees a 0-size
    // viewport, while the zero-size scissor below still clips every
    // fragment -- confinement is the scissor's invariant.)
    const SceneViewportRect sceneRect = effectiveSceneViewport(extent);
    VkViewport viewport{};
    viewport.x = static_cast<float>(sceneRect.x);
    viewport.y = static_cast<float>(sceneRect.y);
    viewport.width = static_cast<float>(std::max(sceneRect.width, 1u));
    viewport.height = static_cast<float>(std::max(sceneRect.height, 1u));
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {sceneRect.x, sceneRect.y};
    scissor.extent = {sceneRect.width, sceneRect.height};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const VkBuffer vertexBuffers[] = {m_vertexBuffer.handle()};
    constexpr VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.layout(), 0, 1,
                            &m_descriptorSets[m_currentFrame], 0, nullptr);

    if (m_postSettings.skyboxEnabled) {
        // Inside-out environment cube at the far plane; depth-write
        // is off, so real geometry always draws over it.
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.skyboxPipeline());
        vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
    }

    m_pipeline.bind(commandBuffer);

    // Draw items arrive sorted by material (RenderQueue), so pipeline
    // switches happen once per material group, not per draw. All
    // pipelines share one layout, so the descriptor binding above stays
    // valid across switches. Unknown materials keep the current pipeline.
    uint32_t boundMaterial = 0;
    bool haveBoundMaterial = false;
    for (const DrawItem& item : drawItems) {
        if (!haveBoundMaterial || item.materialId != boundMaterial) {
            if (const VkPipeline pipeline = m_pipeline.pipelineFor(item.materialId);
                pipeline != VK_NULL_HANDLE) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            }
            boundMaterial = item.materialId;
            haveBoundMaterial = true;
        }
        const DrawPushConstants pushConstants{
            item.model, item.baseColor, item.pbrParams,
            glm::ivec4{item.textureIndices[0], item.textureIndices[1], item.textureIndices[2],
                       item.textureIndices[3]}};
        vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(DrawPushConstants), &pushConstants);
        vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);

    // --- Bloom chain, then the composite into the swapchain image --
    recordBloomChain(commandBuffer);
    recordComposite(commandBuffer, imageIndex);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to record command buffer");
    }
}

bool Renderer::drawFrame(const glm::mat4& view, const glm::mat4& projection,
                         std::span<const DrawItem> drawItems, const FrameLighting& lighting) {
    const VkDevice device = m_context.device();

    vkWaitForFences(device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(device, m_swapchain.handle(), UINT64_MAX,
                                                   m_imageAvailableSemaphores[m_currentFrame],
                                                   VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Renderer: failed to acquire swapchain image");
    }

    // Only reset the fence once we know a frame will actually be submitted,
    // otherwise a failed acquire would leave the fence unsignaled forever.
    vkResetFences(device, 1, &m_inFlightFences[m_currentFrame]);

    updateUniformBuffer(view, projection, lighting);

    VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrame];
    vkResetCommandBuffer(commandBuffer, 0);
    recordCommandBuffer(commandBuffer, imageIndex, drawItems, lighting.cascades, lighting.cascadeCount,
                        lighting.spotShadowEnabled);

    const VkSemaphore waitSemaphores[] = {m_imageAvailableSemaphores[m_currentFrame]};
    const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphores[imageIndex]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(m_context.graphicsQueue(), 1, &submitInfo, m_inFlightFences[m_currentFrame]) !=
        VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to submit draw command buffer");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    const VkSwapchainKHR swapchains[] = {m_swapchain.handle()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(m_context.presentQueue(), &presentInfo);

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
        m_window.wasFramebufferResized()) {
        m_window.resetFramebufferResizedFlag();
        recreateSwapchain();
    } else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to present swapchain image");
    }

    m_currentFrame = (m_currentFrame + 1) % m_framesInFlight;
    return true;
}

void Renderer::recreateSwapchain() {
    int width = 0;
    int height = 0;
    m_window.getFramebufferSize(width, height);
    // A minimized window reports a 0x0 framebuffer; wait until it is
    // restored instead of trying to build a zero-sized swapchain.
    while (width == 0 || height == 0) {
        m_window.getFramebufferSize(width, height);
        Window::waitEvents();
    }

    m_context.waitIdle();
    m_swapchain.recreate();
    // The new swapchain may have a different image count, and the old
    // presentations are finished (device idle), so rebuild these safely.
    createRenderFinishedSemaphores();
    // HDR + bloom targets are extent-sized; rebuild them for the new
    // framebuffer (the scene pass itself is extent-independent).
    destroyPostResources();
    createHdrTargets();
    createPostResources();

    const VkExtent2D extent = m_swapchain.extent();
    ENGINE_LOG_INFO("swapchain recreated for " << extent.width << 'x' << extent.height << " framebuffer");
}

} // namespace engine
