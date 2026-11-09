#pragma once

#include <vulkan/vulkan.h>

#include "Core/PresentationMode.h"

#include <cstddef>
#include <vector>

namespace engine {

class VulkanContext;
class Window;

// ---------------------------------------------------------------------------
// swapchain
//
// Owns the swapchain and everything whose lifetime/size is tied to it:
// color image views, the depth image, the render pass, and one framebuffer
// per swapchain image. recreate() rebuilds the size-dependent resources
// after a window resize; the render pass itself only depends on the image
// formats, so it is created once and survives recreation — the graphics
// pipeline built against it therefore stays valid across resizes.
// ---------------------------------------------------------------------------
struct PresentationSettings {
    PresentationMode mode = PresentationMode::Auto;
    bool vsync = true;
};

class Swapchain {
public:
    // The requested mode/vsync pair is resolved against the modes the
    // surface actually supports; presentMode() reports the result.
    Swapchain(VulkanContext& context, const Window& window, PresentationSettings presentation);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&&) = delete;
    Swapchain& operator=(Swapchain&&) = delete;

    // Rebuilds the swapchain for the window's current framebuffer size.
    // The caller must ensure the device is idle (Renderer does).
    void recreate();

    [[nodiscard]] VkFormat imageFormat() const { return m_imageFormat; }
    [[nodiscard]] VkFormat depthFormat() const { return m_depthFormat; }
    [[nodiscard]] VkExtent2D extent() const { return m_extent; }
    [[nodiscard]] VkRenderPass renderPass() const { return m_renderPass; }
    [[nodiscard]] size_t imageCount() const { return m_images.size(); }
    [[nodiscard]] VkFramebuffer framebuffer(size_t index) const { return m_framebuffers.at(index); }
    [[nodiscard]] VkSwapchainKHR handle() const { return m_swapchain; }
    [[nodiscard]] const PresentationSettings& presentationSettings() const { return m_presentation; }
    [[nodiscard]] VkPresentModeKHR presentMode() const { return m_presentMode; }

    // Pure mode resolution, separated from Vulkan creation for testing.
    [[nodiscard]] static VkPresentModeKHR selectPresentMode(PresentationMode requested, bool vsync,
                                                            const std::vector<VkPresentModeKHR>& availableModes);
    [[nodiscard]] static const char* presentModeName(VkPresentModeKHR mode);

private:
    void createSwapchain(VkSwapchainKHR oldSwapchain);
    void createImageViews();
    void createDepthResources();
    void createRenderPass();
    void createFramebuffers();
    void cleanupSwapchainResources();

    [[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    [[nodiscard]] VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    [[nodiscard]] VkFormat findDepthFormat() const;
    [[nodiscard]] VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                               VkFormatFeatureFlags features) const;

    VulkanContext& m_context;
    const Window& m_window;
    PresentationSettings m_presentation;
    VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_images; // owned by the swapchain, never destroyed manually
    VkFormat m_imageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};

    std::vector<VkImageView> m_imageViews;

    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
};

} // namespace engine
