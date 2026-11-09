#include "Graphics/Renderer.h"

#include "Geometry/Cube.h"
#include "Graphics/GraphicsPipeline.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Platform/Window.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace engine {

Renderer::Renderer(VulkanContext& context, Window& window, Swapchain& swapchain, GraphicsPipeline& pipeline)
    : m_context(context), m_window(window), m_swapchain(swapchain), m_pipeline(pipeline) {
    createVertexBuffer();
    createIndexBuffer();
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
    // m_vertexBuffer / m_indexBuffer / m_uniformBuffers clean themselves up.
}

void Renderer::createCommandBuffers() {
    m_commandBuffers.resize(kMaxFramesInFlight);

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
    m_imageAvailableSemaphores.resize(kMaxFramesInFlight);
    m_inFlightFences.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first frame must not deadlock

    const VkDevice device = m_context.device();
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
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
    m_uniformBuffers.reserve(kMaxFramesInFlight);
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        Buffer buffer(m_context, bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        buffer.map(); // persistently mapped; rewritten each frame
        m_uniformBuffers.push_back(std::move(buffer));
    }
}

void Renderer::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = static_cast<uint32_t>(kMaxFramesInFlight);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = static_cast<uint32_t>(kMaxFramesInFlight);

    if (vkCreateDescriptorPool(m_context.device(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to create descriptor pool");
    }
}

void Renderer::createDescriptorSets() {
    const std::vector<VkDescriptorSetLayout> layouts(kMaxFramesInFlight, m_pipeline.descriptorSetLayout());

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();

    m_descriptorSets.resize(kMaxFramesInFlight);
    if (vkAllocateDescriptorSets(m_context.device(), &allocateInfo, m_descriptorSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to allocate descriptor sets");
    }

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[i].handle();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSets[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_context.device(), 1, &write, 0, nullptr);
    }
}

void Renderer::updateUniformBuffer(const UniformBufferObject& ubo) {
    void* mapped = m_uniformBuffers[m_currentFrame].mappedData();
    std::memcpy(mapped, &ubo, sizeof(ubo));
}

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to begin recording command buffer");
    }

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

    vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);

    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Renderer: failed to record command buffer");
    }
}

void Renderer::drawFrame(const UniformBufferObject& ubo) {
    const VkDevice device = m_context.device();

    vkWaitForFences(device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(device, m_swapchain.handle(), UINT64_MAX,
                                                   m_imageAvailableSemaphores[m_currentFrame],
                                                   VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Renderer: failed to acquire swapchain image");
    }

    // Only reset the fence once we know a frame will actually be submitted,
    // otherwise a failed acquire would leave the fence unsignaled forever.
    vkResetFences(device, 1, &m_inFlightFences[m_currentFrame]);

    updateUniformBuffer(ubo);

    VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrame];
    vkResetCommandBuffer(commandBuffer, 0);
    recordCommandBuffer(commandBuffer, imageIndex);

    const VkSemaphore waitSemaphores[] = {m_imageAvailableSemaphores[m_currentFrame]};
    const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphores[m_currentFrame]};

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

    m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
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
}

} // namespace engine
