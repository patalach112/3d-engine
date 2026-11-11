#include "Platform/Window.h"

#include "Platform/Input.h"

#include <stdexcept>

namespace engine {

Window::Window(int width, int height, const char* title, bool resizable, bool captureCursor)
    : m_width(width), m_height(height) {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("Window: failed to initialize GLFW");
    }

    // No OpenGL/legacy context: Vulkan creates its own device and surface.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (m_window == nullptr) {
        glfwTerminate();
        throw std::runtime_error("Window: failed to create GLFW window");
    }

    glfwSetWindowUserPointer(m_window, this);

    // Record both sizes up front: logical (mouse coordinates) and
    // framebuffer pixels (render target). They only diverge under
    // DPI scaling, but the editor layout keys off the framebuffer.
    glfwGetWindowSize(m_window, &m_width, &m_height);
    glfwGetFramebufferSize(m_window, &m_framebufferWidth, &m_framebufferHeight);
    // Keep tiny/editor windows usable instead of letting panels be
    // dragged to degenerate sizes the layout cannot honour.
    glfwSetWindowSizeLimits(m_window, 640, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);

    // Capture the cursor during window initialisation. In disabled-cursor
    // mode GLFW reports unbounded virtual mouse movement, which is what
    // permits unrestricted 360-degree free look without the pointer being
    // stopped by an OS window border.
    setCursorCaptured(captureCursor);

    glfwSetFramebufferSizeCallback(m_window, &Window::framebufferResizeCallback);
    glfwSetWindowSizeCallback(m_window, &Window::windowSizeCallback);
    glfwSetKeyCallback(m_window, &Window::keyCallback);
    glfwSetCharCallback(m_window, &Window::charCallback);
    glfwSetMouseButtonCallback(m_window, &Window::mouseButtonCallback);
    glfwSetCursorPosCallback(m_window, &Window::cursorPositionCallback);
    glfwSetScrollCallback(m_window, &Window::scrollCallback);
    glfwSetWindowFocusCallback(m_window, &Window::windowFocusCallback);
}

Window::~Window() {
    if (m_window != nullptr) {
        for (void* cursor : m_cursors) {
            if (cursor != nullptr) {
                glfwDestroyCursor(static_cast<GLFWcursor*>(cursor));
            }
        }
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

// ---------------------------------------------------------------------------
// WindowStartupPump
// ---------------------------------------------------------------------------
WindowStartupPump::WindowStartupPump(Window* window, float intervalSeconds) : m_window(window) {
    setInterval(intervalSeconds);
}

void WindowStartupPump::setInterval(float intervalSeconds) {
    m_intervalSeconds = intervalSeconds > 0.0f ? intervalSeconds : 0.0f;
}

void WindowStartupPump::pumpNow() {
    if (m_window == nullptr) {
        return;
    }
    m_window->pollEvents();
    m_lastPump = std::chrono::steady_clock::now();
    m_hasPumped = true;
    ++m_pumpCount;
}

void WindowStartupPump::maybePump() {
    if (m_window == nullptr) {
        return;
    }
    if (m_hasPumped && m_intervalSeconds > 0.0f) {
        const auto elapsed = std::chrono::steady_clock::now() - m_lastPump;
        if (elapsed < std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<float>(m_intervalSeconds))) {
            return; // throttled: startup stages call this freely
        }
    }
    pumpNow();
}

void Window::setCursorCaptured(bool captured) {
    m_cursorCaptured = captured;
    if (m_window != nullptr) {
        glfwSetInputMode(m_window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (!captured) {
            // Re-apply the editor cursor shape suppressed while captured.
            const CursorShape shape = m_cursorShape;
            m_cursorShape = CursorShape::Arrow;
            setCursorShape(shape);
        }
    }
}

void Window::setClipboardText(const std::string& text) {
    if (m_window != nullptr) {
        glfwSetClipboardString(m_window, text.c_str());
    }
}

std::string Window::clipboardText() const {
    if (m_window == nullptr) {
        return {};
    }
    const char* text = glfwGetClipboardString(m_window);
    return text != nullptr ? std::string(text) : std::string{};
}

void Window::setCursorShape(CursorShape shape) {
    if (m_window == nullptr || shape == m_cursorShape) {
        return;
    }
    m_cursorShape = shape;
    if (m_cursorCaptured) {
        return; // hidden cursor while captured; shape applies on release
    }
    const size_t index = static_cast<size_t>(shape);
    if (m_cursors[index] == nullptr) {
        GLFWcursor* cursor = nullptr;
        switch (shape) {
        case CursorShape::Arrow: break;
        case CursorShape::IBeam: cursor = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR); break;
        case CursorShape::ResizeEastWest: cursor = glfwCreateStandardCursor(GLFW_RESIZE_EW_CURSOR); break;
        case CursorShape::ResizeNorthSouth: cursor = glfwCreateStandardCursor(GLFW_RESIZE_NS_CURSOR); break;
        }
        m_cursors[index] = cursor;
    }
    glfwSetCursor(m_window, static_cast<GLFWcursor*>(m_cursors[index]));
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

Window* Window::fromGlfwWindow(GLFWwindow* window) {
    return static_cast<Window*>(glfwGetWindowUserPointer(window));
}

void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    if (Window* self = fromGlfwWindow(window); self != nullptr) {
        self->m_framebufferWidth = width;
        self->m_framebufferHeight = height;
        self->m_framebufferResized = true;
    }
}

void Window::windowSizeCallback(GLFWwindow* window, int width, int height) {
    if (Window* self = fromGlfwWindow(window); self != nullptr) {
        self->m_width = width;
        self->m_height = height;
    }
}

void Window::keyCallback(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
    if (Window* self = fromGlfwWindow(window); self != nullptr && self->m_inputHandler != nullptr) {
        self->m_inputHandler->onKeyEvent(key, action);
    }
}

void Window::charCallback(GLFWwindow* window, unsigned int codepoint) {
    if (Window* self = fromGlfwWindow(window); self != nullptr && self->m_inputHandler != nullptr) {
        self->m_inputHandler->onCharEvent(codepoint);
    }
}

void Window::mouseButtonCallback(GLFWwindow* window, int button, int action, int /*mods*/) {
    if (Window* self = fromGlfwWindow(window); self != nullptr && self->m_inputHandler != nullptr) {
        self->m_inputHandler->onMouseButtonEvent(button, action);
    }
}

void Window::cursorPositionCallback(GLFWwindow* window, double xPosition, double yPosition) {
    if (Window* self = fromGlfwWindow(window); self != nullptr && self->m_inputHandler != nullptr) {
        self->m_inputHandler->onCursorPositionEvent(xPosition, yPosition);
    }
}

void Window::scrollCallback(GLFWwindow* window, double xOffset, double yOffset) {
    if (Window* self = fromGlfwWindow(window); self != nullptr && self->m_inputHandler != nullptr) {
        self->m_inputHandler->onScrollEvent(xOffset, yOffset);
    }
}

void Window::windowFocusCallback(GLFWwindow* window, int focused) {
    if (Window* self = fromGlfwWindow(window); self != nullptr && self->m_inputHandler != nullptr) {
        self->m_inputHandler->onFocusChanged(focused == GLFW_TRUE);
    }
}

} // namespace engine
