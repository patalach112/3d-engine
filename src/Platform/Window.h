#pragma once

// GLFW_INCLUDE_VULKAN makes GLFW pull in <vulkan/vulkan.h> and expose the
// Vulkan surface helpers; GLFW_INCLUDE_NONE stops it from also including
// the legacy OpenGL headers, which this engine deliberately never uses.
#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Window
//
// Thin RAII wrapper around a GLFW window created without any graphics API
// context (GLFW_NO_API): Vulkan owns all rendering. The class also owns the
// GLFW library lifetime for the application (init on construction,
// terminate on destruction), so exactly one Window may exist at a time.
// ---------------------------------------------------------------------------
class Window {
public:
    Window(int width, int height, const char* title, bool resizable = true);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] bool shouldClose() const;
    void requestClose();
    void pollEvents() const;
    static void waitEvents();

    void getFramebufferSize(int& width, int& height) const;

    // Creates the Vulkan surface for this window. Throws on failure.
    [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;

    // Instance extensions required by GLFW to present to this window.
    static std::vector<const char*> requiredInstanceExtensions();

    [[nodiscard]] bool wasFramebufferResized() const { return m_framebufferResized; }
    void resetFramebufferResizedFlag() { m_framebufferResized = false; }

    [[nodiscard]] bool isEscapePressed() const;

    [[nodiscard]] GLFWwindow* handle() const { return m_window; }
    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_framebufferResized = false;
};

} // namespace engine
