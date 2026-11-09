#include "Platform/Window.h"

#include <stdexcept>

namespace engine {

Window::Window(int width, int height, const char* title) : m_width(width), m_height(height) {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("Window: failed to initialize GLFW");
    }

    // No OpenGL/legacy context: Vulkan creates its own device and surface.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (m_window == nullptr) {
        glfwTerminate();
        throw std::runtime_error("Window: failed to create GLFW window");
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, &Window::framebufferResizeCallback);
}

Window::~Window() {
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
    }
    glfwTerminate();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window) == GLFW_TRUE;
}

void Window::requestClose() {
    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

void Window::pollEvents() const {
    glfwPollEvents();
}

void Window::waitEvents() {
    glfwWaitEvents();
}

void Window::getFramebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(m_window, &width, &height);
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, m_window, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Window: failed to create Vulkan window surface");
    }
    return surface;
}

std::vector<const char*> Window::requiredInstanceExtensions() {
    uint32_t count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    if (extensions == nullptr) {
        throw std::runtime_error("Window: GLFW does not report Vulkan instance extensions "
                                 "(Vulkan may not be available on this system)");
    }
    return {extensions, extensions + count};
}

bool Window::isEscapePressed() const {
    return glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
}

void Window::framebufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self != nullptr) {
        self->m_framebufferResized = true;
    }
}

} // namespace engine
