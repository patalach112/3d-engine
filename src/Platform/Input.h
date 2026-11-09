#pragma once

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>

namespace engine {

class Window;

// Engine-level key codes. The rest of the engine only ever sees these;
// translation from GLFW codes happens inside Input (Platform layer), so
// no GLFW constant leaks into Core/Graphics code.
enum class Key : uint8_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Space,
    Escape,
    Enter,
    Tab,
    Backspace,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    Up,
    Down,
    Left,
    Right,
    Count
};

enum class MouseButton : uint8_t {
    Left = 0,
    Right,
    Middle,
    Count
};

// Input tuning that does not belong to a single camera (see [Input] in
// config/engine.ini).
struct InputConfig {
    // Grab and hide the cursor at startup (first-person mouse look).
    // Afterwards: click the window to capture, Esc to release.
    bool captureMouseOnStart = true;
    bool captureMouseOnClick = true;
};

// ---------------------------------------------------------------------------
// Input
//
// Event-driven input state for one window. GLFW callbacks arrive through
// the Window (which owns the GLFW window and forwards events here); this
// class translates them into engine-level state that any system can poll:
//
//   * held state          — isKeyDown() / isMouseButtonDown()
//   * per-frame transitions — wasKeyPressed() / wasKeyReleased() / ...
//   * mouse motion          — mousePosition(), mouseDelta(), scrollDelta()
//
// Frame protocol (driven by the Application's event-polling step):
//   1. beginFrame() clears per-frame data (transitions, deltas),
//   2. Window::pollEvents() lets GLFW fire callbacks into this object,
//   3. engine systems query state during update/render.
// ---------------------------------------------------------------------------
class Input {
public:
    explicit Input(Window& window);
    ~Input();

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) = delete;
    Input& operator=(Input&&) = delete;

    // Clears per-frame transitions and motion deltas. Call immediately
    // before Window::pollEvents() each frame.
    void beginFrame();

    // Clears all held/transitional state (e.g. after focus loss, so keys
    // don't appear stuck when the window regains focus).
    void resetState();

    [[nodiscard]] bool isKeyDown(Key key) const;
    [[nodiscard]] bool wasKeyPressed(Key key) const;
    [[nodiscard]] bool wasKeyReleased(Key key) const;

    [[nodiscard]] bool isMouseButtonDown(MouseButton button) const;
    [[nodiscard]] bool wasMouseButtonPressed(MouseButton button) const;
    [[nodiscard]] bool wasMouseButtonReleased(MouseButton button) const;

    // Absolute cursor position in pixels (window coordinates).
    [[nodiscard]] glm::vec2 mousePosition() const { return m_mousePosition; }
    // Cursor movement accumulated during this frame's event polling, in
    // pixels. Positive Y means the cursor moved down.
    [[nodiscard]] glm::vec2 mouseDelta() const { return m_mouseDelta; }
    // Scroll wheel movement accumulated this frame.
    [[nodiscard]] glm::vec2 scrollDelta() const { return m_scrollDelta; }

    // Cursor capture hides the cursor and reports unbounded movement —
    // what first-person mouse look needs.
    void setMouseCaptured(bool captured);
    [[nodiscard]] bool isMouseCaptured() const { return m_mouseCaptured; }

    // --- Event entry points (called by Window's GLFW callbacks) -----------
    void onKeyEvent(int glfwKey, int glfwAction);
    void onMouseButtonEvent(int glfwButton, int glfwAction);
    void onCursorPositionEvent(double xPosition, double yPosition);
    void onScrollEvent(double xOffset, double yOffset);
    void onFocusChanged(bool focused);

private:
    static constexpr size_t kKeyCount = static_cast<size_t>(Key::Count);
    static constexpr size_t kButtonCount = static_cast<size_t>(MouseButton::Count);

    Window& m_window;

    std::array<bool, kKeyCount> m_keysDown{};
    std::array<bool, kKeyCount> m_keysPressed{};
    std::array<bool, kKeyCount> m_keysReleased{};

    std::array<bool, kButtonCount> m_buttonsDown{};
    std::array<bool, kButtonCount> m_buttonsPressed{};
    std::array<bool, kButtonCount> m_buttonsReleased{};

    glm::vec2 m_mousePosition{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
    glm::vec2 m_scrollDelta{0.0f};
    bool m_hasMousePosition = false;
    bool m_mouseCaptured = false;
};

} // namespace engine
