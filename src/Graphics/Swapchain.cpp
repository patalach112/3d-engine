#include "Graphics/Swapchain.h"

#include "Core/Logger.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/VulkanHelpers.h"
#include "Platform/Window.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace engine {

Swapchain::Swapchain(VulkanContext& context, const Window& window, PresentationSettings presentation)
    : m_context(context), m_window(window), m_presentation(presentation) {
    createSwapchain(VK_NULL_HANDLE);
    createImageViews();
    m_depthFormat = findDepthFormat();
    createDepthResources();
    createRenderPass();
    createFramebuffers();
}

Swapchain::~Swapchain() {
    cleanupSwapchainResources();
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_context.device(), m_renderPass, nullptr);
    }
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_context.device(), m_swapchain, nullptr);
    }
}

void Swapchain::recreate() {
    const VkSwapchainKHR oldSwapchain = m_swapchain;

    // Hand the old swapchain to the driver so it can recycle its images,
    // then release the retired handle.
    createSwapchain(oldSwapchain);
    cleanupSwapchainResources();
    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_context.device(), oldSwapchain, nullptr);
    }

    createImageViews();
    createDepthResources();
    createFramebuffers();
}

void Swapchain::cleanupSwapchainResources() {
    VkDevice device = m_context.device();

    for (const VkFramebuffer framebuffer : m_framebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    m_framebuffers.clear();

    if (m_depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    if (m_depthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_depthMemory, nullptr);
        m_depthMemory = VK_NULL_HANDLE;
    }

    for (const VkImageView view : m_imageViews) {
        vkDestroyImageView(device, view, nullptr);
    }
    m_imageViews.clear();
}

void Swapchain::createSwapchain(VkSwapchainKHR oldSwapchain) {
    const SwapChainSupportDetails support = m_context.querySwapChainSupport(m_context.physicalDevice());

    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode =
        selectPresentMode(m_presentation.mode, m_presentation.vsync, support.presentModes);
    m_presentMode = presentMode;
    const VkExtent2D extent = chooseExtent(support.capabilities);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, support.capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_context.surface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const QueueFamilyIndices& families = m_context.queueFamilies();
    const std::array<uint32_t, 2> familyIndices = {families.graphicsFamily.value(),
                                                   families.presentFamily.value()};
    if (families.graphicsFamily != families.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<uint32_t>(familyIndices.size());
        createInfo.pQueueFamilyIndices = familyIndices.data();
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    if (vkCreateSwapchainKHR(m_context.device(), &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        throw std::runtime_error("swapchain: failed to create swapchain");
    }

    uint32_t actualImageCount = 0;
    vkGetSwapchainImagesKHR(m_context.device(), m_swapchain, &actualImageCount, nullptr);
    m_images.resize(actualImageCount);
    vkGetSwapchainImagesKHR(m_context.device(), m_swapchain, &actualImageCount, m_images.data());

    m_imageFormat = surfaceFormat.format;
    m_extent = extent;

    ENGINE_LOG_DEBUG("swapchain: " << extent.width << 'x' << extent.height << ", " << m_images.size()
                                    << " images, format " << static_cast<int>(surfaceFormat.format)
                                    << ", requested present mode "
                                    << presentationModeName(m_presentation.mode) << " (vsync "
                                    << (m_presentation.vsync ? "on" : "off") << "), using "
                                    << presentModeName(presentMode));
}

void Swapchain::createImageViews() {
    m_imageViews.resize(m_images.size());
    for (size_t i = 0; i < m_images.size(); ++i) {
        m_imageViews[i] =
            helpers::createImageView(m_context, m_images[i], m_imageFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

void Swapchain::createDepthResources() {
    helpers::createImage(m_context, m_extent.width, m_extent.height, m_depthFormat, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         m_depthImage, m_depthMemory);
    m_depthView = helpers::createImageView(m_context, m_depthImage, m_depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Swapchain::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_imageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = m_depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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

    // Synchronize with the presentation engine: don't start writing color
    // or depth before the swapchain image is available.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(m_context.device(), &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        throw std::runtime_error("swapchain: failed to create render pass");
    }
}

void Swapchain::createFramebuffers() {
    m_framebuffers.resize(m_imageViews.size());
    for (size_t i = 0; i < m_imageViews.size(); ++i) {
        const std::array<VkImageView, 2> attachments = {m_imageViews[i], m_depthView};

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = m_extent.width;
        framebufferInfo.height = m_extent.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(m_context.device(), &framebufferInfo, nullptr, &m_framebuffers[i]) !=
            VK_SUCCESS) {
            throw std::runtime_error("swapchain: failed to create framebuffer");
        }
    }
}

VkSurfaceFormatKHR Swapchain::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR Swapchain::selectPresentMode(PresentationMode requested, bool vsync,
                                              const std::vector<VkPresentModeKHR>& availableModes) {
    const auto available = [&availableModes](VkPresentModeKHR mode) {
        return std::find(availableModes.begin(), availableModes.end(), mode) != availableModes.end();
    };

    switch (requested) {
    case PresentationMode::Fifo:
        // FIFO is guaranteed by the Vulkan specification.
        return VK_PRESENT_MODE_FIFO_KHR;
    case PresentationMode::FifoRelaxed:
        return available(VK_PRESENT_MODE_FIFO_RELAXED_KHR) ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                                                           : VK_PRESENT_MODE_FIFO_KHR;
    case PresentationMode::Mailbox:
        return available(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                                                      : VK_PRESENT_MODE_FIFO_KHR;
    case PresentationMode::Immediate:
        if (available(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        return available(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                                                      : VK_PRESENT_MODE_FIFO_KHR;
    case PresentationMode::Auto:
        break;
    }

    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    // Auto with vsync off keeps the historical preference: present as soon
    // as possible; use Mailbox when the platform does not offer Immediate.
    if (available(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (available(VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (available(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

const char* Swapchain::presentModeName(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR: return "SHARED_DEMAND_REFRESH";
    case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR: return "SHARED_CONTINUOUS_REFRESH";
    default: return "UNKNOWN";
    }
}

VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    int width = 0;
    int height = 0;
    m_window.getFramebufferSize(width, height);

    VkExtent2D extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height);
    return extent;
}

VkFormat Swapchain::findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                        VkFormatFeatureFlags features) const {
    for (const VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_context.physicalDevice(), format, &properties);
        if (tiling == VK_IMAGE_TILING_LINEAR && (properties.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == VK_IMAGE_TILING_OPTIMAL && (properties.optimalTilingFeatures & features) == features) {
            return format;
        }
    }
    throw std::runtime_error("swapchain: failed to find a supported format");
}

VkFormat Swapchain::findDepthFormat() const {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

} // namespace engine
