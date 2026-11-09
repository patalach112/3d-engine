#include "Graphics/Renderer.h"

#include "Core/Logger.h"
#include "Geometry/Cube.h"
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

Renderer::Renderer(VulkanContext& context, Window& window, Swapchain& swapchain, GraphicsPipeline& pipeline,
                   uint32_t maxFramesInFlight)
    : m_context(context), m_window(window), m_swapchain(swapchain), m_pipeline(pipeline),
      m_hud(context, swapchain),
      m_framesInFlight(std::clamp(maxFramesInFlight, 1u, static_cast<uint32_t>(kMaxFramesInFlight))) {
    createVertexBuffer();
    createIndexBuffer();
    createShadowResources();
    createUniformBuffers();
    createDescriptorPool();
    createDescriptorSets();
    createCommandBuffers();
    createSyncObjects();
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
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    }
    if (m_shadowFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, m_shadowFramebuffer, nullptr);
    }
    if (m_shadowRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_shadowRenderPass, nullptr);
    }
    if (m_shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_shadowSampler, nullptr);
    }
    if (m_shadowView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_shadowView, nullptr);
    }
    if (m_shadowImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_shadowImage, nullptr);
    }
    if (m_shadowMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_shadowMemory, nullptr);
    }
    // m_vertexBuffer / m_indexBuffer / m_uniformBuffers clean themselves up.
}

void Renderer::createShadowResources() {
    const VkDevice device = m_context.device();
    constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

    helpers::createImage(m_context, kShadowMapSize, kShadowMapSize, kShadowFormat, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_shadowImage, m_shadowMemory);
    m_shadowView = helpers::createImageView(m_context, m_shadowImage, kShadowFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

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

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_shadowRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &m_shadowView;
    framebufferInfo.width = kShadowMapSize;
    framebufferInfo.height = kShadowMapSize;
    framebufferInfo.layers = 1;
    if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &m_shadowFramebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create shadow framebuffer");
    }

    m_pipeline.createShadowPipeline(m_shadowRenderPass);
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
    poolSizes[1].descriptorCount = m_framesInFlight;

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

        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.sampler = m_shadowSampler;
        shadowInfo.imageView = m_shadowView;
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descriptorSets[i];
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufferInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_descriptorSets[i];
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &shadowInfo;

        vkUpdateDescriptorSets(m_context.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    }
}

void Renderer::updateUniformBuffer(const glm::mat4& view, const glm::mat4& projection) {
    UniformBufferObject ubo{};
    ubo.view = view;
    ubo.proj = projection;
    ubo.lightViewProj = directionalLightViewProjection(directionalLightDirection(), shadowSceneCenter(),
                                                       kShadowSceneExtent, kShadowLightDistance,
                                                       kShadowNearPlane, kShadowFarPlane);
    const glm::vec3 lightDirection = directionalLightDirection();
    ubo.lightDirection = glm::vec4{lightDirection, 0.0f};
    void* mapped = m_uniformBuffers[m_currentFrame].mappedData();
    std::memcpy(mapped, &ubo, sizeof(ubo));
}

void Renderer::recordShadowPass(VkCommandBuffer commandBuffer, std::span<const DrawItem> drawItems) {
    // Depth-only pass from the key light's point of view. Every draw item
    // (floor included) writes its light-space depth; the main pass then
    // tests fragments against this map. Rendered first, into a resource
    // whose render-pass transitions hand it to the fragment shader.
    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_shadowRenderPass;
    renderPassInfo.framebuffer = m_shadowFramebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {kShadowMapSize, kShadowMapSize};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

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

    for (const DrawItem& item : drawItems) {
        // Same 112-byte push payload as the main pass; shadow.vert reads
        // the model matrix from its head.
        const DrawPushConstants pushConstants{item.model, item.baseColor, item.specular, item.surface};
        vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(DrawPushConstants), &pushConstants);
        vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);
}

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                                   std::span<const DrawItem> drawItems) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to begin recording command buffer");
    }

    // Shadow pass first: it fills the depth texture the main pass samples.
    recordShadowPass(commandBuffer, drawItems);

    const VkExtent2D extent = m_swapchain.extent();

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {0.05f, 0.05f, 0.08f, 1.0f}; // dark blue-grey background
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

    m_pipeline.bind(commandBuffer);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const VkBuffer vertexBuffers[] = {m_vertexBuffer.handle()};
    constexpr VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.layout(), 0, 1,
                            &m_descriptorSets[m_currentFrame], 0, nullptr);

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
        const DrawPushConstants pushConstants{item.model, item.baseColor, item.specular, item.surface};
        vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(DrawPushConstants), &pushConstants);
        vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
    }

    // FPS HUD overlay, drawn last inside the same render pass.
    m_hud.record(commandBuffer, extent);

    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to record command buffer");
    }
}

bool Renderer::drawFrame(const glm::mat4& view, const glm::mat4& projection,
                         std::span<const DrawItem> drawItems) {
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

    updateUniformBuffer(view, projection);

    VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrame];
    vkResetCommandBuffer(commandBuffer, 0);
    recordCommandBuffer(commandBuffer, imageIndex, drawItems);

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

    const VkExtent2D extent = m_swapchain.extent();
    ENGINE_LOG_INFO("swapchain recreated for " << extent.width << 'x' << extent.height << " framebuffer");
}

} // namespace engine
