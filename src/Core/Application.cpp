#include "Core/Application.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>

namespace engine {
namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr const char* kWindowTitle = "3D Engine — Phase 1: Vulkan Cube";

} // namespace

Application::Application()
    : m_window(kWindowWidth, kWindowHeight, kWindowTitle),
      m_context(m_window),
      m_swapchain(m_context, m_window),
      m_pipeline(m_context, m_swapchain),
      m_renderer(m_context, m_window, m_swapchain, m_pipeline) {}

Application::~Application() {
    // Make sure no frame is still using the resources that member
    // destruction is about to release (Renderer, buffers, swapchain...).
    m_context.waitIdle();
}

void Application::run() {
    const auto startTime = std::chrono::steady_clock::now();

    while (!m_window.shouldClose()) {
        m_window.pollEvents();

        if (m_window.isEscapePressed()) {
            m_window.requestClose();
            break;
        }

        const float elapsedSeconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();

        m_renderer.drawFrame(computeTransforms(elapsedSeconds));
    }

    // Drain the GPU before teardown so destructors never free resources
    // that are still in flight.
    m_context.waitIdle();
}

UniformBufferObject Application::computeTransforms(float elapsedSeconds) const {
    UniformBufferObject ubo;

    // Continuous rotation around multiple axes at different speeds, so the
    // cube tumbles smoothly and every colored face becomes visible.
    glm::mat4 model{1.0f};
    model = glm::rotate(model, elapsedSeconds * glm::radians(55.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, elapsedSeconds * glm::radians(32.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, elapsedSeconds * glm::radians(18.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.model = model;

    // Static camera looking at the origin from +Z.
    ubo.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                           glm::vec3(0.0f, 1.0f, 0.0f));

    const VkExtent2D extent = m_swapchain.extent();
    const float aspect = extent.height > 0
                             ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                             : 1.0f;
    ubo.proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);

    // GLM was designed for OpenGL, whose clip-space Y axis points down
    // compared to Vulkan. Flip it, otherwise the image is upside down
    // (the pipeline's clockwise front-face setting compensates winding).
    ubo.proj[1][1] *= -1.0f;

    return ubo;
}

} // namespace engine
