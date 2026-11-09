#include "Platform/Input.h"

#include "Platform/Window.h"

// GLFW codes are translated here in the Platform layer; this include is
// deliberately not part of Input.h so Core code stays GLFW-free.
#include <GLFW/glfw3.h>

namespace engine {
namespace {

// Translates a GLFW key code to the engine-level Key. GLFW letters/digits
// are contiguous ranges, which keeps this mapping compact.
Key translateKey(int glfwKey) {
    if (glfwKey >= GLFW_KEY_A && glfwKey <= GLFW_KEY_Z) {
        return static_cast<Key>(static_cast<uint8_t>(Key::A) + static_cast<uint8_t>(glfwKey - GLFW_KEY_A));
    }
    if (glfwKey >= GLFW_KEY_0 && glfwKey <= GLFW_KEY_9) {
        return static_cast<Key>(static_cast<uint8_t>(Key::Num0) + static_cast<uint8_t>(glfwKey - GLFW_KEY_0));
    }
    switch (glfwKey) {
    case GLFW_KEY_SPACE: return Key::Space;
    case GLFW_KEY_ESCAPE: return Key::Escape;
    case GLFW_KEY_ENTER: return Key::Enter;
    case GLFW_KEY_TAB: return Key::Tab;
    case GLFW_KEY_BACKSPACE: return Key::Backspace;
    case GLFW_KEY_LEFT_SHIFT: return Key::LeftShift;
    case GLFW_KEY_RIGHT_SHIFT: return Key::RightShift;
    case GLFW_KEY_LEFT_CONTROL: return Key::LeftControl;
    case GLFW_KEY_RIGHT_CONTROL: return Key::RightControl;
    case GLFW_KEY_LEFT_ALT: return Key::LeftAlt;
    case GLFW_KEY_RIGHT_ALT: return Key::RightAlt;
    case GLFW_KEY_UP: return Key::Up;
    case GLFW_KEY_DOWN: return Key::Down;
    case GLFW_KEY_LEFT: return Key::Left;
    case GLFW_KEY_RIGHT: return Key::Right;
    default: return Key::Unknown;
    }
}

MouseButton translateMouseButton(int glfwButton) {
    switch (glfwButton) {
    case GLFW_MOUSE_BUTTON_LEFT: return MouseButton::Left;
    case GLFW_MOUSE_BUTTON_RIGHT: return MouseButton::Right;
    case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::Middle;
    default: return MouseButton::Count; // unsupported button: ignored by callers
    }
}

} // namespace

Input::Input(Window& window) : m_window(window) {
    m_window.setInputHandler(this);
}

Input::~Input() {
    if (m_mouseCaptured) {
        setMouseCaptured(false);
    }
    m_window.setInputHandler(nullptr);
}

void Input::beginFrame() {
    m_keysPressed.fill(false);
    m_keysReleased.fill(false);
    m_buttonsPressed.fill(false);
    m_buttonsReleased.fill(false);
    m_mouseDelta = glm::vec2{0.0f};
    m_scrollDelta = glm::vec2{0.0f};
}

void Input::resetState() {
    m_keysDown.fill(false);
    m_buttonsDown.fill(false);
    beginFrame();
}

bool Input::isKeyDown(Key key) const {
    return m_keysDown[static_cast<size_t>(key)];
}

bool Input::wasKeyPressed(Key key) const {
    return m_keysPressed[static_cast<size_t>(key)];
}

bool Input::wasKeyReleased(Key key) const {
    return m_keysReleased[static_cast<size_t>(key)];
}

bool Input::isMouseButtonDown(MouseButton button) const {
    return m_buttonsDown[static_cast<size_t>(button)];
}

bool Input::wasMouseButtonPressed(MouseButton button) const {
    return m_buttonsPressed[static_cast<size_t>(button)];
}

bool Input::wasMouseButtonReleased(MouseButton button) const {
    return m_buttonsReleased[static_cast<size_t>(button)];
}

void Input::setMouseCaptured(bool captured) {
    if (m_mouseCaptured == captured) {
        return;
    }
    m_mouseCaptured = captured;
    glfwSetInputMode(m_window.handle(), GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // The first cursor event after (un)capturing only sets the baseline,
    // so the camera doesn't jump by the accumulated offset.
    m_hasMousePosition = false;
}

void Input::onKeyEvent(int glfwKey, int glfwAction) {
    const Key key = translateKey(glfwKey);
    if (key == Key::Unknown) {
        return;
    }
    const size_t index = static_cast<size_t>(key);
    if (glfwAction == GLFW_PRESS) {
        if (!m_keysDown[index]) {
            m_keysPressed[index] = true;
        }
        m_keysDown[index] = true;
    } else if (glfwAction == GLFW_RELEASE) {
        m_keysDown[index] = false;
        m_keysReleased[index] = true;
    }
    // GLFW_REPEAT is intentionally ignored: held state already covers it.
}

void Input::onMouseButtonEvent(int glfwButton, int glfwAction) {
    const MouseButton button = translateMouseButton(glfwButton);
    if (button == MouseButton::Count) {
        return;
    }
    const size_t index = static_cast<size_t>(button);
    if (glfwAction == GLFW_PRESS) {
        if (!m_buttonsDown[index]) {
            m_buttonsPressed[index] = true;
        }
        m_buttonsDown[index] = true;
    } else if (glfwAction == GLFW_RELEASE) {
        m_buttonsDown[index] = false;
        m_buttonsReleased[index] = true;
    }
}

void Input::onCursorPositionEvent(double xPosition, double yPosition) {
    const glm::vec2 position{static_cast<float>(xPosition), static_cast<float>(yPosition)};
    if (m_hasMousePosition) {
        m_mouseDelta += position - m_mousePosition;
    }
    m_mousePosition = position;
    m_hasMousePosition = true;
}

void Input::onScrollEvent(double xOffset, double yOffset) {
    m_scrollDelta += glm::vec2{static_cast<float>(xOffset), static_cast<float>(yOffset)};
}

void Input::onFocusChanged(bool focused) {
    if (!focused) {
        resetState();
    }
}

} // namespace engine
