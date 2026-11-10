#pragma once
#include "Core/Export.h"

// GLFW_INCLUDE_VULKAN makes GLFW pull in <vulkan/vulkan.h> and expose the
// Vulkan surface helpers; GLFW_INCLUDE_NONE stops it from also including
// the legacy OpenGL headers, which this engine deliberately never uses.
#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <vector>

namespace engine {

class Input;

// ---------------------------------------------------------------------------
// Window
//
// Thin RAII wrapper around a GLFW window created without any graphics API
// context (GLFW_NO_API): Vulkan owns all rendering. The class also owns the
// GLFW library lifetime for the application (init on construction,
// terminate on destruction), so exactly one Window may exist at a time.
// ---------------------------------------------------------------------------
class ENGINE_PLATFORM_API Window {
public:
    Window(int width, int height, const char* title, bool resizable = true, bool captureCursor = true);
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

    // Applies GLFW's captured/free cursor modes. Capture is established
    // during window creation when requested, rather than being deferred
    // until the rest of the application has finished constructing.
    void setCursorCaptured(bool captured);
    [[nodiscard]] bool isCursorCaptured() const { return m_cursorCaptured; }
    [[nodiscard]] static constexpr int cursorModeForCapture(bool captured) {
        return captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
    }

    // Registers the input system that GLFW key/mouse callbacks are
    // forwarded to (nullptr detaches). Owned by the caller; the Window
    // must outlive the registration (Application's member order does).
    void setInputHandler(Input* handler) { m_inputHandler = handler; }

    [[nodiscard]] GLFWwindow* handle() const { return m_window; }
    // Logical window size (GLFW window coordinates: the units mouse
    // positions arrive in). Updated by the window-size callback on
    // every resize -- previously these stayed at the creation size
    // forever, which froze the editor layout at its startup geometry.
    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }
    // Physical framebuffer size in pixels (the render/UI surface).
    // Differs from the logical size under DPI/content scaling.
    [[nodiscard]] int framebufferWidth() const { return m_framebufferWidth; }
    [[nodiscard]] int framebufferHeight() const { return m_framebufferHeight; }
    // Framebuffer pixels per logical window pixel (>1 on HiDPI).
    // Multiply GLFW mouse positions by this to address the
    // framebuffer-sized UI batch; 1 when either size is unknown.
    [[nodiscard]] float uiCoordinateScale() const {
        return m_width > 0 ? static_cast<float>(m_framebufferWidth) / static_cast<float>(m_width)
                           : 1.0f;
    }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void windowSizeCallback(GLFWwindow* window, int width, int height);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void cursorPositionCallback(GLFWwindow* window, double xPosition, double yPosition);
    static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);
    static void windowFocusCallback(GLFWwindow* window, int focused);

    // The Window instance associated with a GLFW window (user pointer).
    static Window* fromGlfwWindow(GLFWwindow* window);

    GLFWwindow* m_window = nullptr;
    Input* m_inputHandler = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_framebufferWidth = 0;
    int m_framebufferHeight = 0;
    bool m_framebufferResized = false;
    bool m_cursorCaptured = false;
};

} // namespace engine
