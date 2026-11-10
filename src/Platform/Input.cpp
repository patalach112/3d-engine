#include "Platform/Input.h"

#include "Platform/Window.h"

// GLFW codes are translated here in the Platform layer; this include is
// deliberately not part of Input.h so Core code stays GLFW-free.
#include <GLFW/glfw3.h>

#include <algorithm>

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

Input::Input(Window& window) : m_window(&window) {
    m_window->setInputHandler(this);
    // Window applies the requested GLFW cursor mode during construction;
    // adopt that state without re-sending the mode or losing the fact that
    // the cursor is already captured at startup.
    m_mouseCaptured = m_window->isCursorCaptured();
    m_hasMousePosition = false;
}

Input::Input() = default;

Input::~Input() {
    if (m_window == nullptr) {
        return; // headless: nothing was ever registered
    }
    if (m_mouseCaptured) {
        setMouseCaptured(false);
    }
    m_window->setInputHandler(nullptr);
}

void Input::beginFrame() {
    m_keysPressed.fill(false);
    m_keysReleased.fill(false);
    m_buttonsPressed.fill(false);
    m_buttonsReleased.fill(false);
    m_mouseDelta = glm::vec2{0.0f};
    m_scrollDelta = glm::vec2{0.0f};
    m_textInput.clear();
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
    if (m_mouseCaptured == captured &&
        (m_window == nullptr || m_window->isCursorCaptured() == captured)) {
        return;
    }
    m_mouseCaptured = captured;
    if (m_window != nullptr) {
        m_window->setCursorCaptured(captured);
    }
    // A mode change can make previously accumulated cursor samples stale.
    // In particular, absolute positions reported just before GLFW switches
    // to its virtual captured cursor must not be replayed as one large
    // look jump on the next frame. The next event only sets the baseline.
    m_mouseDelta = glm::vec2{0.0f};
    m_hasMousePosition = false;
}

void Input::injectKeyState(Key key, bool down) {
    if (key == Key::Unknown || key == Key::Count) {
        return;
    }
    const size_t index = static_cast<size_t>(key);
    if (down) {
        if (!m_keysDown[index]) {
            m_keysPressed[index] = true;
        }
        m_keysDown[index] = true;
    } else if (m_keysDown[index]) {
        m_keysDown[index] = false;
        m_keysReleased[index] = true;
    }
}

void Input::injectMouseButtonState(MouseButton button, bool down) {
    if (button == MouseButton::Count) {
        return;
    }
    const size_t index = static_cast<size_t>(button);
    if (down) {
        if (!m_buttonsDown[index]) {
            m_buttonsPressed[index] = true;
        }
        m_buttonsDown[index] = true;
    } else if (m_buttonsDown[index]) {
        m_buttonsDown[index] = false;
        m_buttonsReleased[index] = true;
    }
}

void Input::injectCursorPosition(glm::vec2 position) {
    if (m_hasMousePosition) {
        m_mouseDelta += position - m_mousePosition;
    }
    m_mousePosition = position;
    m_hasMousePosition = true;
}

void Input::injectScrollDelta(glm::vec2 delta) {
    m_scrollDelta += delta;
}

void Input::onCharEvent(unsigned int codepoint) {
    // Encode the Unicode codepoint as UTF-8 into this frame's text
    // buffer. Control codepoints never arrive here (GLFW reports
    // them as key events instead), and surrogates/out-of-range
    // values are dropped rather than encoded invalidly.
    if (codepoint == 0 || codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        return;
    }
    char encoded[4];
    size_t length = 0;
    if (codepoint < 0x80u) {
        encoded[length++] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800u) {
        encoded[length++] = static_cast<char>(0xC0u | (codepoint >> 6));
        encoded[length++] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint < 0x10000u) {
        encoded[length++] = static_cast<char>(0xE0u | (codepoint >> 12));
        encoded[length++] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[length++] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
    } else {
        encoded[length++] = static_cast<char>(0xF0u | (codepoint >> 18));
        encoded[length++] = static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
        encoded[length++] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[length++] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
    }
    m_textInput.append(encoded, length);
}

void Input::onKeyEvent(int glfwKey, int glfwAction) {
    // GLFW_REPEAT is intentionally not forwarded: held state covers it.
    if (glfwAction == GLFW_PRESS) {
        injectKeyState(translateKey(glfwKey), true);
    } else if (glfwAction == GLFW_RELEASE) {
        injectKeyState(translateKey(glfwKey), false);
    }
}

void Input::onMouseButtonEvent(int glfwButton, int glfwAction) {
    if (glfwAction == GLFW_PRESS) {
        injectMouseButtonState(translateMouseButton(glfwButton), true);
    } else if (glfwAction == GLFW_RELEASE) {
        injectMouseButtonState(translateMouseButton(glfwButton), false);
    }
}

void Input::onCursorPositionEvent(double xPosition, double yPosition) {
    injectCursorPosition(glm::vec2{static_cast<float>(xPosition), static_cast<float>(yPosition)});
}

void Input::onScrollEvent(double xOffset, double yOffset) {
    injectScrollDelta(glm::vec2{static_cast<float>(xOffset), static_cast<float>(yOffset)});
}

void Input::bindAction(std::string_view name, Key key) {
    ActionBinding& binding = m_actions[std::string{name}];
    if (std::find(binding.keys.begin(), binding.keys.end(), key) == binding.keys.end()) {
        binding.keys.push_back(key);
    }
}

void Input::bindActionButton(std::string_view name, MouseButton button) {
    ActionBinding& binding = m_actions[std::string{name}];
    if (std::find(binding.buttons.begin(), binding.buttons.end(), button) == binding.buttons.end()) {
        binding.buttons.push_back(button);
    }
}

void Input::clearActionBindings(std::string_view name) {
    m_actions.erase(std::string{name});
}

bool Input::isActionDown(std::string_view name) const {
    const auto found = m_actions.find(std::string{name});
    if (found == m_actions.end()) {
        return false;
    }
    for (const Key key : found->second.keys) {
        if (isKeyDown(key)) {
            return true;
        }
    }
    for (const MouseButton button : found->second.buttons) {
        if (isMouseButtonDown(button)) {
            return true;
        }
    }
    return false;
}

bool Input::wasActionPressed(std::string_view name) const {
    const auto found = m_actions.find(std::string{name});
    if (found == m_actions.end()) {
        return false;
    }
    for (const Key key : found->second.keys) {
        if (wasKeyPressed(key)) {
            return true;
        }
    }
    for (const MouseButton button : found->second.buttons) {
        if (wasMouseButtonPressed(button)) {
            return true;
        }
    }
    return false;
}

bool Input::wasActionReleased(std::string_view name) const {
    const auto found = m_actions.find(std::string{name});
    if (found == m_actions.end()) {
        return false;
    }
    for (const Key key : found->second.keys) {
        if (wasKeyReleased(key)) {
            return true;
        }
    }
    for (const MouseButton button : found->second.buttons) {
        if (wasMouseButtonReleased(button)) {
            return true;
        }
    }
    return false;
}

void Input::bindAxis(std::string_view name, Key negative, Key positive) {
    m_axes[std::string{name}] = AxisBinding{negative, positive};
}

void Input::clearAxisBinding(std::string_view name) {
    m_axes.erase(std::string{name});
}

float Input::axisValue(std::string_view name) const {
    const auto found = m_axes.find(std::string{name});
    if (found == m_axes.end()) {
        return 0.0f;
    }
    const float value = (isKeyDown(found->second.positive) ? 1.0f : 0.0f) -
                        (isKeyDown(found->second.negative) ? 1.0f : 0.0f);
    return value;
}

void Input::onFocusChanged(bool focused) {
    if (!focused) {
        resetState();
        // A captured cursor must never survive focus loss: the user
        // Alt-Tabbed away precisely to get their mouse back, and a
        // hidden locked cursor on return reads as "the app froze".
        if (m_mouseCaptured) {
            setMouseCaptured(false);
        }
    }
}

} // namespace engine
